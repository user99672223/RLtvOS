// socket.cpp — see socket.h.
#include "socket.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include "linux_abi.h"
#include "overlay.h"
#include "poll.h"

namespace rlk {

namespace {

std::atomic<uint64_t> g_sock_ino {0x60000};

// The abstract namespace (names without their leading NUL).
struct AbstractNames {
    std::mutex mu;
    std::map<std::string, std::weak_ptr<UnixSocket>> names;
    static AbstractNames& get() {
        static AbstractNames t;
        return t;
    }
};

// Locks two sockets in address order (one may equal the other).
struct PairLock {
    PairLock(std::mutex& a, std::mutex& b) : a_(a), b_(b), same_(&a == &b) {
        if (same_) {
            a_.lock();
        } else if (&a < &b) {
            a_.lock();
            b_.lock();
        } else {
            b_.lock();
            a_.lock();
        }
    }
    ~PairLock() {
        a_.unlock();
        if (!same_) b_.unlock();
    }
    std::mutex& a_;
    std::mutex& b_;
    bool same_;
};

bool is_stream(int type) {
    return type == lx::sock_stream;
}

}  // namespace

// ---- lifecycle ------------------------------------------------------------------------

UnixSocket::UnixSocket(int type, SockCreds owner) : ino(g_sock_ino.fetch_add(1)), type_(type), owner_(owner) {}

UnixSocket::~UnixSocket() = default;

void UnixSocket::pair(int type, SockCreds owner, std::shared_ptr<UnixSocket>& a, std::shared_ptr<UnixSocket>& b) {
    a = std::make_shared<UnixSocket>(type, owner);
    b = std::make_shared<UnixSocket>(type, owner);
    a->peer_ = b;
    b->peer_ = a;
    a->had_peer_ = b->had_peer_ = true;
    a->peer_creds_ = b->peer_creds_ = owner;
}

// Both mutexes held: the connection between this and `peer` ends (close or
// the death of a listener's queued connection).
void UnixSocket::disconnect_locked_pair(const std::shared_ptr<UnixSocket>& peer) {
    if (peer) {
        peer->peer_gone_ = true;
        peer->rx_eof_ = true;
        peer->peer_.reset();
        peer->rq_.wake();
        peer->wq_.wake();
    }
    peer_.reset();
    peer_gone_ = true;
    rx_eof_ = true;
}

void UnixSocket::close() {
    std::shared_ptr<UnixSocket> peer;
    std::deque<std::shared_ptr<UnixSocket>> backlog;
    std::string abstract;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (closed_) return;
        closed_ = true;
        peer = peer_;
        backlog.swap(backlog_);
        listening_ = false;
        if (!name_.empty() && name_[0] == '@') abstract = name_.substr(1);
    }
    if (peer) {
        PairLock lk(mu_, peer->mu_);
        if (peer_ == peer) disconnect_locked_pair(peer);
    }
    for (auto& s : backlog) {  // never accepted: their clients see a reset
        s->close();
    }
    if (!abstract.empty()) {
        auto& t = AbstractNames::get();
        std::lock_guard<std::mutex> lk(t.mu);
        auto it = t.names.find(abstract);
        if (it != t.names.end() && it->second.lock().get() == this) t.names.erase(it);
    }
    rq_.wake();
    wq_.wake();
}

// ---- names --------------------------------------------------------------------------

int UnixSocket::bind_abstract(const std::string& name) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!name_.empty()) return -lx::einval;
    auto& t = AbstractNames::get();
    std::lock_guard<std::mutex> tl(t.mu);
    auto it = t.names.find(name);
    if (it != t.names.end() && !it->second.expired()) return -lx::eaddrinuse;
    t.names[name] = shared_from_this();
    name_ = "@" + name;
    return 0;
}

int UnixSocket::bind_path(const std::string& path, const std::shared_ptr<TmpNode>& node) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!name_.empty()) return -lx::einval;
    name_ = path;
    node_ = node;
    node->socket = shared_from_this();
    return 0;
}

std::shared_ptr<UnixSocket> UnixSocket::find_abstract(const std::string& name) {
    auto& t = AbstractNames::get();
    std::lock_guard<std::mutex> lk(t.mu);
    auto it = t.names.find(name);
    return it == t.names.end() ? nullptr : it->second.lock();
}

std::string UnixSocket::name() {
    std::lock_guard<std::mutex> lk(mu_);
    return name_;
}

std::string UnixSocket::peer_name() {
    std::lock_guard<std::mutex> lk(mu_);
    return peer_name_;
}

// ---- connections ------------------------------------------------------------------

int UnixSocket::listen(int backlog) {
    std::lock_guard<std::mutex> lk(mu_);
    if (type_ == lx::sock_dgram) return -lx::eopnotsupp;
    if (name_.empty()) return -lx::einval;
    if (peer_ || had_peer_) return -lx::einval;
    listening_ = true;
    backlog_max_ = std::max(1, std::min(backlog, 4096));
    return 0;
}

int UnixSocket::connect(const std::shared_ptr<UnixSocket>& target, bool nonblock) {
    if (!target || target->type_ != type_) return -lx::eprototype;
    if (type_ == lx::sock_dgram) {
        std::lock_guard<std::mutex> lk(mu_);
        if (listening_) return -lx::einval;
        peer_ = target;
        peer_name_ = target->name();
        peer_creds_ = target->owner_;
        had_peer_ = true;
        peer_gone_ = false;
        rx_eof_ = false;
        return 0;
    }
    Waiter local;
    Waiter* w = CurrentWaiter() ? CurrentWaiter() : &local;
    for (;;) {
        target->wq_.add(w);
        const uint64_t gen = w->prepare();
        {
            PairLock lk(mu_, target->mu_);
            if (peer_ || had_peer_) {
                target->wq_.remove(w);
                return -lx::eisconn;
            }
            if (listening_) {
                target->wq_.remove(w);
                return -lx::einval;
            }
            if (!target->listening_ || target->closed_) {
                target->wq_.remove(w);
                return -lx::econnrefused;
            }
            if ((int)target->backlog_.size() < target->backlog_max_) {
                auto server = std::make_shared<UnixSocket>(type_, target->owner_);
                server->name_ = target->name_;
                server->passcred = target->passcred;
                server->sndbuf = target->sndbuf;
                server->rcvbuf = target->rcvbuf;
                server->peer_ = shared_from_this();
                server->peer_creds_ = owner_;
                server->had_peer_ = true;
                peer_ = server;
                peer_name_ = target->name_;
                peer_creds_ = target->owner_;
                had_peer_ = true;
                target->backlog_.push_back(server);
                target->rq_.wake();
                target->wq_.remove(w);
                return 0;
            }
            if (nonblock) {
                target->wq_.remove(w);
                return -lx::eagain;
            }
        }
        const auto r = w->wait(gen, 0);
        target->wq_.remove(w);
        if (r == Waiter::Result::Interrupted) return -lx::eintr;
    }
}

std::shared_ptr<UnixSocket> UnixSocket::accept(bool nonblock, int& err) {
    Waiter local;
    Waiter* w = CurrentWaiter() ? CurrentWaiter() : &local;
    for (;;) {
        rq_.add(w);
        const uint64_t gen = w->prepare();
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!listening_) {
                rq_.remove(w);
                err = lx::einval;
                return nullptr;
            }
            if (!backlog_.empty()) {
                auto s = backlog_.front();
                backlog_.pop_front();
                rq_.remove(w);
                wq_.wake();  // room for a waiting connector
                err = 0;
                return s;
            }
            if (nonblock) {
                rq_.remove(w);
                err = lx::eagain;
                return nullptr;
            }
        }
        const auto r = w->wait(gen, 0);
        rq_.remove(w);
        if (r == Waiter::Result::Interrupted) {
            err = lx::eintr;
            return nullptr;
        }
    }
}

int UnixSocket::shutdown(int how) {
    if (how < lx::shut_rd || how > lx::shut_rdwr) return -lx::einval;
    std::shared_ptr<UnixSocket> peer;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!had_peer_ && type_ != lx::sock_dgram) return -lx::enotconn;
        peer = peer_;
    }
    if (peer) {
        PairLock lk(mu_, peer->mu_);
        if (how == lx::shut_rd || how == lx::shut_rdwr) rd_shut_ = true;
        if (how == lx::shut_wr || how == lx::shut_rdwr) {
            wr_shut_ = true;
            if (peer_ == peer) {
                peer->rx_eof_ = true;
                peer->rq_.wake();
            }
        }
    } else {
        std::lock_guard<std::mutex> lk(mu_);
        if (how == lx::shut_rd || how == lx::shut_rdwr) rd_shut_ = true;
        if (how == lx::shut_wr || how == lx::shut_rdwr) wr_shut_ = true;
    }
    rq_.wake();
    wq_.wake();
    return 0;
}

// ---- data ----------------------------------------------------------------------------

int64_t UnixSocket::send(const uint8_t* data, size_t len, std::vector<std::shared_ptr<OpenFile>> fds, int flags,
                         const std::shared_ptr<UnixSocket>& to, bool nonblock) {
    if (flags & lx::msg_oob) return -lx::eopnotsupp;
    Waiter local;
    Waiter* w = CurrentWaiter() ? CurrentWaiter() : &local;
    size_t done = 0;
    for (;;) {
        std::shared_ptr<UnixSocket> dst = to;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (wr_shut_) return done ? (int64_t)done : -lx::epipe;
            if (!dst) dst = peer_;
            if (!dst) {
                if (type_ == lx::sock_dgram) return -lx::enotconn;
                return peer_gone_ || had_peer_ ? -lx::epipe : -lx::enotconn;
            }
        }
        wq_.add(w);
        const uint64_t gen = w->prepare();
        {
            PairLock lk(mu_, dst->mu_);
            if (wr_shut_) {
                wq_.remove(w);
                return done ? (int64_t)done : -lx::epipe;
            }
            if (!to && peer_ != dst) {  // the peer changed under us: start over
                wq_.remove(w);
                continue;
            }
            if (dst->closed_ || dst->rd_shut_ || (type_ != lx::sock_dgram && dst->peer_.get() != this)) {
                wq_.remove(w);
                if (type_ == lx::sock_dgram) return -lx::econnrefused;
                return done ? (int64_t)done : -lx::epipe;
            }
            const size_t room = dst->rcvbuf > dst->rx_bytes_ ? dst->rcvbuf - dst->rx_bytes_ : 0;
            if (is_stream(type_)) {
                const size_t want = len - done;
                if (want == 0) {
                    wq_.remove(w);
                    return (int64_t)done;
                }
                if (room > 0) {
                    const size_t n = std::min(want, room);
                    SockMsg m;
                    m.data.assign(data + done, data + done + n);
                    m.creds = owner_;
                    m.from = name_;
                    if (!fds.empty()) m.fds = std::move(fds);
                    fds.clear();
                    dst->rx_.push_back(std::move(m));
                    dst->rx_bytes_ += n;
                    done += n;
                    dst->rq_.wake();
                    wq_.remove(w);
                    if (done == len) return (int64_t)done;
                    continue;  // more to send: wait for room next round if needed
                }
            } else {
                if (len > dst->rcvbuf) {
                    wq_.remove(w);
                    return -lx::emsgsize;
                }
                if (room >= len || dst->rx_.empty()) {
                    SockMsg m;
                    m.data.assign(data, data + len);
                    m.creds = owner_;
                    m.from = name_;
                    m.fds = std::move(fds);
                    dst->rx_.push_back(std::move(m));
                    dst->rx_bytes_ += len;
                    dst->rq_.wake();
                    wq_.remove(w);
                    return (int64_t)len;
                }
            }
            if (nonblock) {
                wq_.remove(w);
                return done ? (int64_t)done : -lx::eagain;
            }
        }
        const auto r = w->wait(gen, 0);
        wq_.remove(w);
        if (r == Waiter::Result::Interrupted) return done ? (int64_t)done : -lx::eintr;
    }
}

int64_t UnixSocket::recv(uint8_t* buf, size_t len, int flags, bool nonblock, RecvInfo* info) {
    if (flags & lx::msg_oob) return -lx::einval;
    const bool peek = flags & lx::msg_peek;
    const bool waitall = (flags & lx::msg_waitall) && is_stream(type_);
    Waiter local;
    Waiter* w = CurrentWaiter() ? CurrentWaiter() : &local;
    size_t total = 0;
    for (;;) {
        rq_.add(w);
        const uint64_t gen = w->prepare();
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (listening_) {
                rq_.remove(w);
                return -lx::einval;
            }
            if (!rx_.empty()) {
                std::shared_ptr<UnixSocket> peer = peer_;
                if (is_stream(type_)) {
                    bool got_fds = false;
                    size_t idx = 0;  // peek: walk without consuming
                    while (total < len && idx < rx_.size()) {
                        SockMsg& m = rx_[idx];
                        if (!m.fds.empty() && total > 0 && got_fds) break;
                        const size_t avail = m.data.size() - m.off;
                        const size_t n = std::min(len - total, avail);
                        memcpy(buf + total, m.data.data() + m.off, n);
                        total += n;
                        if (info && !info->has_creds) {
                            info->creds = m.creds;
                            info->has_creds = true;
                        }
                        if (!m.fds.empty()) {
                            if (info) info->fds = peek ? m.fds : std::move(m.fds);
                            if (!peek) m.fds.clear();
                            got_fds = true;
                        }
                        if (peek) {
                            idx++;
                        } else {
                            m.off += n;
                            rx_bytes_ -= n;
                            if (m.off == m.data.size()) rx_.pop_front();
                        }
                        if (got_fds) break;  // one fd set per call
                    }
                    if (!peek && peer) peer->wq_.wake();
                    if (waitall && total < len && !rx_eof_ && !rd_shut_ && !nonblock && !got_fds) {
                        // keep collecting
                    } else {
                        rq_.remove(w);
                        return (int64_t)total;
                    }
                } else {
                    SockMsg& m = rx_.front();
                    const size_t n = std::min(len, m.data.size());
                    memcpy(buf, m.data.data(), n);
                    if (info) {
                        info->creds = m.creds;
                        info->has_creds = true;
                        info->from = m.from;
                        info->truncated = n < m.data.size();
                        info->fds = peek ? m.fds : std::move(m.fds);
                    }
                    const size_t size = m.data.size();
                    if (!peek) {
                        rx_bytes_ -= size;
                        rx_.pop_front();
                        if (peer) peer->wq_.wake();
                    }
                    rq_.remove(w);
                    return (int64_t)((flags & lx::msg_trunc) ? size : n);
                }
            } else {
                if (total > 0) {  // waitall: partial data and nothing more yet
                    if (rx_eof_ || rd_shut_ || nonblock) {
                        rq_.remove(w);
                        return (int64_t)total;
                    }
                } else {
                    if (rd_shut_ || rx_eof_) {
                        rq_.remove(w);
                        return 0;
                    }
                    if (type_ != lx::sock_dgram && !had_peer_) {
                        rq_.remove(w);
                        return -lx::einval;
                    }
                    if (nonblock) {
                        rq_.remove(w);
                        return -lx::eagain;
                    }
                }
            }
        }
        const auto r = w->wait(gen, 0);
        rq_.remove(w);
        if (r == Waiter::Result::Interrupted) return total ? (int64_t)total : -lx::eintr;
    }
}

// ---- state ----------------------------------------------------------------------------

unsigned UnixSocket::poll_locked() {
    unsigned m = 0;
    if (so_error_) m |= lx::pollerr;
    if (listening_) {
        if (!backlog_.empty()) m |= lx::pollin | lx::pollrdnorm;
        return m;
    }
    if (!rx_.empty()) m |= lx::pollin | lx::pollrdnorm;
    if (rx_eof_ || rd_shut_) m |= lx::pollin | lx::pollrdnorm | lx::pollrdhup;
    if (peer_gone_ && type_ != lx::sock_dgram && had_peer_) m |= lx::pollhup;
    if (rd_shut_ && wr_shut_) m |= lx::pollhup;
    if (!wr_shut_) {
        if (peer_) {
            // the peer's byte count is read without its lock: a stale value only costs a spurious wakeup
            if (peer_->rx_bytes_ < peer_->rcvbuf) m |= lx::pollout | lx::pollwrnorm;
        } else {
            m |= lx::pollout | lx::pollwrnorm;  // unconnected dgram, or peer gone (write → EPIPE)
        }
    }
    return m;
}

unsigned UnixSocket::poll(PollTable* pt) {
    if (pt) {
        pt->add(rq_);
        pt->add(wq_);
    }
    std::lock_guard<std::mutex> lk(mu_);
    return poll_locked();
}

bool UnixSocket::is_listening() {
    std::lock_guard<std::mutex> lk(mu_);
    return listening_;
}

bool UnixSocket::is_connected() {
    std::lock_guard<std::mutex> lk(mu_);
    return peer_ != nullptr;
}

SockCreds UnixSocket::peer_creds() {
    std::lock_guard<std::mutex> lk(mu_);
    return peer_creds_;
}

bool UnixSocket::has_peer_creds() {
    std::lock_guard<std::mutex> lk(mu_);
    return had_peer_;
}

int64_t UnixSocket::queued_bytes() {
    std::lock_guard<std::mutex> lk(mu_);
    if (is_stream(type_)) return (int64_t)rx_bytes_;
    return rx_.empty() ? 0 : (int64_t)rx_.front().data.size();
}

int UnixSocket::take_error() {
    std::lock_guard<std::mutex> lk(mu_);
    int e = so_error_;
    so_error_ = 0;
    return e;
}

void UnixSocket::set_error(int err) {
    std::lock_guard<std::mutex> lk(mu_);
    so_error_ = err;
    rq_.wake();
    wq_.wake();
}

// ---- SocketFile ---------------------------------------------------------------------

SocketFile::SocketFile(std::shared_ptr<UnixSocket> s, int flags) : sock_(std::move(s)) {
    path = "socket:[" + std::to_string(sock_->ino) + "]";
    oflags = lx::o_rdwr | ((flags & lx::sock_nonblock) ? lx::o_nonblock : 0);
}

SocketFile::~SocketFile() {
    sock_->close();
}

int64_t SocketFile::read(void* buf, size_t len) {
    if (len == 0) return 0;
    return sock_->recv(static_cast<uint8_t*>(buf), len, 0, nonblock(), nullptr);
}

int64_t SocketFile::write(const void* buf, size_t len) {
    return sock_->send(static_cast<const uint8_t*>(buf), len, {}, 0, nullptr, nonblock());
}

int SocketFile::fstat(lx::stat& st) {
    FillStat(st, lx::s_ifsock | 0777, 0, 0, sock_->ino);
    st.st_dev = 0x8;
    return 0;
}

int64_t SocketFile::ioctl(unsigned req, uint64_t arg) {
    if (req == lx::fionread && arg) {
        *reinterpret_cast<int32_t*>(arg) = (int32_t)sock_->queued_bytes();
        return 0;
    }
    if (req == lx::fionbio && arg) {
        if (*reinterpret_cast<const int32_t*>(arg)) {
            oflags |= lx::o_nonblock;
        } else {
            oflags &= ~lx::o_nonblock;
        }
        return 0;
    }
    return -lx::enotty;
}

unsigned SocketFile::poll(PollTable* pt) {
    return sock_->poll(pt);
}

}  // namespace rlk
