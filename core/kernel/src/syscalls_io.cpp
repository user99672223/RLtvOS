// syscalls_io.cpp — the I/O-multiplexing and IPC half of the syscall table:
// poll/ppoll, select/pselect6, epoll, eventfd, AF_UNIX sockets (socket.h),
// interval timers.
#include "syscalls.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "overlay.h"
#include "poll.h"
#include "socket.h"

namespace rlk {

namespace {

// ---- poll / select ---------------------------------------------------------------------

// A poll-family call returned -EINTR: restart it transparently only when no
// handler runs (Linux never restarts these after a handler, SA_RESTART or not).
void poll_interrupted(Sc& c) {
    c.t.restartable = true;
    c.t.no_handler_restart = true;
}

// Installs a temporary signal mask for ppoll/pselect6/epoll_pwait; the
// epilogue (or the handler's frame) restores the old one (like rt_sigsuspend).
int apply_sigmask(Sc& c, uint64_t maskp, uint64_t size) {
    if (!maskp) return 0;
    if (size != 8) return -lx::einval;
    const uint64_t old = c.t.sigmask;
    c.t.sigmask = *reinterpret_cast<const uint64_t*>(maskp) & ~(lx::sigbit(lx::sigkill) | lx::sigbit(lx::sigstop));
    c.t.saved_mask_valid = true;
    c.t.saved_mask = old;
    return 0;
}

int64_t do_poll(Sc& c, lx::pollfd* fds, uint64_t nfds, int64_t deadline_ns, bool nowait) {
    if (nfds > 4096) return -lx::einval;
    if (nfds && !fds) return -lx::efault;
    std::vector<PollEntry> entries;
    std::vector<size_t> index;
    entries.reserve(nfds);
    for (uint64_t i = 0; i < nfds; i++) {
        fds[i].revents = 0;
        if (fds[i].fd < 0) continue;
        entries.push_back({fd_get(c, fds[i].fd), (unsigned)(uint16_t)fds[i].events, 0});
        index.push_back((size_t)i);
    }
    int64_t r = DoPoll(entries, deadline_ns, nowait);
    for (size_t k = 0; k < entries.size(); k++) fds[index[k]].revents = (int16_t)entries[k].revents;
    if (r == -lx::eintr) poll_interrupted(c);
    return r;
}

int64_t sys_poll(Sc& c) {
    c.fmt("0x%llx, %llu, %d", (unsigned long long)c.a[0], (unsigned long long)c.a[1], (int)c.a[2]);
    const int timeout = (int)c.a[2];
    const int64_t deadline = timeout > 0 ? MonotonicNs() + (int64_t)timeout * 1000000LL : 0;
    return do_poll(c, reinterpret_cast<lx::pollfd*>(c.a[0]), c.a[1], deadline, timeout == 0);
}

int64_t sys_ppoll(Sc& c) {
    c.fmt("0x%llx, %llu, 0x%llx, 0x%llx, %llu", (unsigned long long)c.a[0], (unsigned long long)c.a[1],
          (unsigned long long)c.a[2], (unsigned long long)c.a[3], (unsigned long long)c.a[4]);
    const auto* ts = reinterpret_cast<const lx::timespec*>(c.a[2]);
    if (ts && (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000LL)) return -lx::einval;
    int e = apply_sigmask(c, c.a[3], c.a[4]);
    if (e) return e;
    const bool nowait = ts && ts->tv_sec == 0 && ts->tv_nsec == 0;
    return do_poll(c, reinterpret_cast<lx::pollfd*>(c.a[0]), c.a[1], deadline_from(ts), nowait);
}

struct FdSets {
    uint64_t* r;
    uint64_t* w;
    uint64_t* e;
};

bool set_has(const uint64_t* s, int fd) {
    return s && (s[fd / 64] >> (fd % 64)) & 1;
}

void set_bit(uint64_t* s, int fd) {
    if (s) s[fd / 64] |= 1ull << (fd % 64);
}

int64_t do_select(Sc& c, int nfds, FdSets sets, int64_t deadline_ns, bool nowait) {
    if (nfds < 0 || nfds > lx::fd_setsize) return -lx::einval;
    std::vector<PollEntry> entries;
    std::vector<int> fdnums;
    for (int fd = 0; fd < nfds; fd++) {
        unsigned ev = 0;
        if (set_has(sets.r, fd)) ev |= lx::pollin | lx::pollrdnorm | lx::pollhup | lx::pollerr;
        if (set_has(sets.w, fd)) ev |= lx::pollout | lx::pollwrnorm | lx::pollerr;
        if (set_has(sets.e, fd)) ev |= lx::pollpri;
        if (!ev) continue;
        auto f = fd_get(c, fd);
        if (!f) return -lx::ebadf;
        entries.push_back({f, ev, 0});
        fdnums.push_back(fd);
    }
    int64_t r = DoPoll(entries, deadline_ns, nowait);
    if (r < 0) {
        if (r == -lx::eintr) poll_interrupted(c);
        return r;
    }
    const size_t words = (size_t)(nfds + 63) / 64;
    for (uint64_t* s : {sets.r, sets.w, sets.e}) {
        if (s) memset(s, 0, words * 8);
    }
    int64_t count = 0;
    for (size_t k = 0; k < entries.size(); k++) {
        const int fd = fdnums[k];
        const unsigned rev = entries[k].revents;
        if (rev & lx::pollnval) return -lx::ebadf;
        if (entries[k].events & lx::pollin && (rev & (lx::pollin | lx::pollrdnorm | lx::pollhup | lx::pollerr))) {
            set_bit(sets.r, fd);
            count++;
        }
        if (entries[k].events & lx::pollout && (rev & (lx::pollout | lx::pollwrnorm | lx::pollerr))) {
            set_bit(sets.w, fd);
            count++;
        }
        if (entries[k].events & lx::pollpri && (rev & lx::pollpri)) {
            set_bit(sets.e, fd);
            count++;
        }
    }
    return count;
}

int64_t sys_select(Sc& c) {
    c.fmt("%d, 0x%llx, 0x%llx, 0x%llx, 0x%llx", (int)c.a[0], (unsigned long long)c.a[1], (unsigned long long)c.a[2],
          (unsigned long long)c.a[3], (unsigned long long)c.a[4]);
    auto* tv = reinterpret_cast<lx::timeval*>(c.a[4]);
    if (tv && (tv->tv_sec < 0 || tv->tv_usec < 0)) return -lx::einval;
    int64_t deadline = 0;
    if (tv) deadline = MonotonicNs() + tv->tv_sec * 1000000000LL + tv->tv_usec * 1000LL;
    const bool nowait = tv && tv->tv_sec == 0 && tv->tv_usec == 0;
    int64_t r = do_select(c, (int)c.a[0], {reinterpret_cast<uint64_t*>(c.a[1]), reinterpret_cast<uint64_t*>(c.a[2]),
                                            reinterpret_cast<uint64_t*>(c.a[3])}, deadline, nowait);
    if (tv && r >= 0) {  // Linux writes the remaining time back
        const int64_t left = std::max<int64_t>(0, deadline - MonotonicNs());
        tv->tv_sec = left / 1000000000LL;
        tv->tv_usec = (left % 1000000000LL) / 1000;
    }
    return r;
}

int64_t sys_pselect6(Sc& c) {
    c.fmt("%d, 0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx", (int)c.a[0], (unsigned long long)c.a[1], (unsigned long long)c.a[2],
          (unsigned long long)c.a[3], (unsigned long long)c.a[4], (unsigned long long)c.a[5]);
    const auto* ts = reinterpret_cast<const lx::timespec*>(c.a[4]);
    if (ts && (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000LL)) return -lx::einval;
    if (c.a[5]) {
        const auto* sig = reinterpret_cast<const uint64_t*>(c.a[5]);  // { const sigset_t* ss; size_t ss_len; }
        int e = apply_sigmask(c, sig[0], sig[1]);
        if (e) return e;
    }
    const bool nowait = ts && ts->tv_sec == 0 && ts->tv_nsec == 0;
    return do_select(c, (int)c.a[0], {reinterpret_cast<uint64_t*>(c.a[1]), reinterpret_cast<uint64_t*>(c.a[2]),
                                       reinterpret_cast<uint64_t*>(c.a[3])}, deadline_from(ts), nowait);
}

// ---- epoll ------------------------------------------------------------------------------

std::shared_ptr<EpollFile> epoll_of(Sc& c, int fd) {
    return std::dynamic_pointer_cast<EpollFile>(fd_get(c, fd));
}

int64_t sys_epoll_create1(Sc& c) {
    c.fmt("0x%llx", (unsigned long long)c.a[0]);
    if (c.a[0] & ~(uint64_t)lx::epoll_cloexec) return -lx::einval;
    return c.p.fds.alloc(std::make_shared<EpollFile>(), (c.a[0] & lx::epoll_cloexec) != 0);
}

int64_t sys_epoll_create(Sc& c) {
    c.fmt("%d", (int)c.a[0]);
    if ((int)c.a[0] <= 0) return -lx::einval;
    return c.p.fds.alloc(std::make_shared<EpollFile>(), false);
}

int64_t sys_epoll_ctl(Sc& c) {
    c.fmt("%d, %d, %d, 0x%llx", c.fd(0), (int)c.a[1], c.fd(2), (unsigned long long)c.a[3]);
    auto ep = epoll_of(c, c.fd(0));
    if (!fd_get(c, c.fd(0))) return -lx::ebadf;
    if (!ep) return -lx::einval;
    auto f = fd_get(c, c.fd(2));
    if (!f) return -lx::ebadf;
    const int op = (int)c.a[1];
    const auto* ev = reinterpret_cast<const lx::epoll_event*>(c.a[3]);
    if (op != lx::epoll_ctl_del && !ev) return -lx::efault;
    uint32_t events = 0;
    uint64_t data = 0;
    if (ev) {
        memcpy(&events, &ev->events, sizeof events);
        memcpy(&data, &ev->data, sizeof data);
    }
    return ep->ctl(op, c.fd(2), f, events, data);
}

int64_t do_epoll_wait(Sc& c, int epfd, lx::epoll_event* events, int max, int64_t deadline_ns, bool nowait) {
    auto ep = epoll_of(c, epfd);
    if (!fd_get(c, epfd)) return -lx::ebadf;
    if (!ep) return -lx::einval;
    if (max <= 0 || !events) return max <= 0 ? -lx::einval : -lx::efault;
    int64_t r = ep->wait(events, max, deadline_ns, nowait);
    if (r == -lx::eintr) poll_interrupted(c);
    return r;
}

int64_t sys_epoll_wait(Sc& c) {
    c.fmt("%d, 0x%llx, %d, %d", c.fd(0), (unsigned long long)c.a[1], (int)c.a[2], (int)c.a[3]);
    const int timeout = (int)c.a[3];
    const int64_t deadline = timeout > 0 ? MonotonicNs() + (int64_t)timeout * 1000000LL : 0;
    return do_epoll_wait(c, c.fd(0), reinterpret_cast<lx::epoll_event*>(c.a[1]), (int)c.a[2], deadline, timeout == 0);
}

int64_t sys_epoll_pwait(Sc& c) {
    c.fmt("%d, 0x%llx, %d, %d, 0x%llx, %llu", c.fd(0), (unsigned long long)c.a[1], (int)c.a[2], (int)c.a[3],
          (unsigned long long)c.a[4], (unsigned long long)c.a[5]);
    int e = apply_sigmask(c, c.a[4], c.a[5]);
    if (e) return e;
    const int timeout = (int)c.a[3];
    const int64_t deadline = timeout > 0 ? MonotonicNs() + (int64_t)timeout * 1000000LL : 0;
    return do_epoll_wait(c, c.fd(0), reinterpret_cast<lx::epoll_event*>(c.a[1]), (int)c.a[2], deadline, timeout == 0);
}

int64_t sys_epoll_pwait2(Sc& c) {
    c.fmt("%d, 0x%llx, %d, 0x%llx, 0x%llx, %llu", c.fd(0), (unsigned long long)c.a[1], (int)c.a[2],
          (unsigned long long)c.a[3], (unsigned long long)c.a[4], (unsigned long long)c.a[5]);
    const auto* ts = reinterpret_cast<const lx::timespec*>(c.a[3]);
    if (ts && (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000LL)) return -lx::einval;
    int e = apply_sigmask(c, c.a[4], c.a[5]);
    if (e) return e;
    const bool nowait = ts && ts->tv_sec == 0 && ts->tv_nsec == 0;
    return do_epoll_wait(c, c.fd(0), reinterpret_cast<lx::epoll_event*>(c.a[1]), (int)c.a[2], deadline_from(ts), nowait);
}

// ---- eventfd ---------------------------------------------------------------------------

int64_t sys_eventfd2(Sc& c) {
    c.fmt("%llu, 0x%llx", (unsigned long long)c.a[0], (unsigned long long)c.a[1]);
    const int flags = (int)c.a[1];
    if (flags & ~(lx::efd_semaphore | lx::efd_cloexec | lx::efd_nonblock)) return -lx::einval;
    return c.p.fds.alloc(std::make_shared<EventFdFile>((uint32_t)c.a[0], flags), (flags & lx::efd_cloexec) != 0);
}

int64_t sys_eventfd(Sc& c) {
    c.fmt("%llu", (unsigned long long)c.a[0]);
    return c.p.fds.alloc(std::make_shared<EventFdFile>((uint32_t)c.a[0], 0), false);
}

// ---- sockets ----------------------------------------------------------------------------

std::shared_ptr<SocketFile> sock_of(Sc& c, int fd, int64_t& err) {
    auto f = fd_get(c, fd);
    if (!f) {
        err = -lx::ebadf;
        return nullptr;
    }
    auto s = std::dynamic_pointer_cast<SocketFile>(f);
    if (!s) err = -lx::enotsock;
    return s;
}

// sockaddr_un → name. "" = unnamed; abstract names lose their leading NUL.
int parse_unix_addr(uint64_t addr, uint64_t len, std::string& name, bool& abstract, bool for_connect) {
    if (!addr) return -lx::efault;
    if (len < 2 || len > sizeof(lx::sockaddr_un)) return -lx::einval;
    const auto* sa = reinterpret_cast<const lx::sockaddr_un*>(addr);
    if (sa->sun_family != lx::af_unix) return for_connect ? -lx::eafnosupport : -lx::einval;
    abstract = false;
    name.clear();
    if (len == 2) return 0;
    const size_t plen = (size_t)len - 2;
    if (sa->sun_path[0] == 0) {
        abstract = true;
        name.assign(sa->sun_path + 1, plen - 1);
        return 0;
    }
    name.assign(sa->sun_path, strnlen(sa->sun_path, plen));
    return 0;
}

// Writes a bound name as sockaddr_un (truncating to *lenp, then setting *lenp
// to the full length, like the kernel).
void write_unix_addr(const std::string& name, uint64_t addr, uint64_t lenp) {
    if (!addr || !lenp) return;
    auto* len = reinterpret_cast<uint32_t*>(lenp);
    lx::sockaddr_un sa {};
    sa.sun_family = lx::af_unix;
    uint32_t full = 2;
    if (!name.empty() && name[0] == '@') {
        const size_t n = std::min<size_t>(name.size() - 1, sizeof sa.sun_path - 1);
        memcpy(sa.sun_path + 1, name.data() + 1, n);
        full = (uint32_t)(3 + n);
    } else if (!name.empty()) {
        const size_t n = std::min<size_t>(name.size(), sizeof sa.sun_path - 1);
        memcpy(sa.sun_path, name.data(), n);
        full = (uint32_t)(3 + n);
    }
    memcpy(reinterpret_cast<void*>(addr), &sa, std::min<uint32_t>(*len, full));
    *len = full;
}

// The socket bound at a name (abstract table or an overlay Sock node).
std::shared_ptr<UnixSocket> find_bound(Sc& c, const std::string& name, bool abstract, int& err) {
    if (abstract) {
        auto s = UnixSocket::find_abstract(name);
        if (!s) err = lx::econnrefused;
        return s;
    }
    if (name.empty()) {
        err = lx::einval;
        return nullptr;
    }
    Lookup l;
    err = K().overlay().lookup(K().resolve_path(c.p, lx::at_fdcwd, name), true, l);
    if (err) return nullptr;
    if ((l.st.st_mode & lx::s_ifmt) != lx::s_ifsock) {
        err = lx::econnrefused;
        return nullptr;
    }
    auto s = l.upper ? l.upper->socket.lock() : nullptr;
    if (!s) err = lx::econnrefused;
    return s;
}

int64_t sys_socket(Sc& c) {
    c.fmt("%d, 0x%llx, %d", (int)c.a[0], (unsigned long long)c.a[1], (int)c.a[2]);
    const int domain = (int)c.a[0], type = (int)c.a[1] & lx::sock_type_mask, flags = (int)c.a[1] & ~lx::sock_type_mask;
    if (domain != lx::af_unix) return -lx::eafnosupport;  // no network on the TV (C4: X11 over unix sockets)
    if (type != lx::sock_stream && type != lx::sock_dgram && type != lx::sock_seqpacket) return -lx::esocktnosupport;
    if ((int)c.a[2] != 0) return -lx::eprotonosupport;
    if (flags & ~(lx::sock_nonblock | lx::sock_cloexec)) return -lx::einval;
    auto s = std::make_shared<UnixSocket>(type, SockCreds {c.p.pid, 1000, 1000});
    return c.p.fds.alloc(std::make_shared<SocketFile>(s, flags), (flags & lx::sock_cloexec) != 0);
}

int64_t sys_socketpair(Sc& c) {
    c.fmt("%d, 0x%llx, %d, 0x%llx", (int)c.a[0], (unsigned long long)c.a[1], (int)c.a[2], (unsigned long long)c.a[3]);
    const int domain = (int)c.a[0], type = (int)c.a[1] & lx::sock_type_mask, flags = (int)c.a[1] & ~lx::sock_type_mask;
    auto* out = reinterpret_cast<int32_t*>(c.a[3]);
    if (!out) return -lx::efault;
    if (domain != lx::af_unix) return -lx::eafnosupport;
    if (type != lx::sock_stream && type != lx::sock_dgram && type != lx::sock_seqpacket) return -lx::esocktnosupport;
    if (flags & ~(lx::sock_nonblock | lx::sock_cloexec)) return -lx::einval;
    std::shared_ptr<UnixSocket> a, b;
    UnixSocket::pair(type, SockCreds {c.p.pid, 1000, 1000}, a, b);
    const bool cloexec = flags & lx::sock_cloexec;
    int fa = c.p.fds.alloc(std::make_shared<SocketFile>(a, flags), cloexec);
    if (fa < 0) return fa;
    int fb = c.p.fds.alloc(std::make_shared<SocketFile>(b, flags), cloexec);
    if (fb < 0) {
        c.p.fds.close(fa);
        return fb;
    }
    out[0] = fa;
    out[1] = fb;
    return 0;
}

int64_t sys_bind(Sc& c) {
    c.fmt("%d, 0x%llx, %llu", c.fd(0), (unsigned long long)c.a[1], (unsigned long long)c.a[2]);
    int64_t err = 0;
    auto sf = sock_of(c, c.fd(0), err);
    if (!sf) return err;
    std::string name;
    bool abstract;
    int e = parse_unix_addr(c.a[1], c.a[2], name, abstract, false);
    if (e) return e;
    if (abstract) return sf->sock()->bind_abstract(name);
    if (name.empty()) return -lx::einval;  // autobind is not provided
    const std::string full = K().resolve_path(c.p, lx::at_fdcwd, name);
    std::shared_ptr<TmpNode> node;
    e = K().overlay().mknod(full, lx::s_ifsock | (0777 & ~c.p.umask), node);
    if (e == lx::eexist) return -lx::eaddrinuse;
    if (e) return -e;
    return sf->sock()->bind_path(full, node);
}

int64_t sys_listen(Sc& c) {
    c.fmt("%d, %d", c.fd(0), (int)c.a[1]);
    int64_t err = 0;
    auto sf = sock_of(c, c.fd(0), err);
    if (!sf) return err;
    return sf->sock()->listen((int)c.a[1]);
}

int64_t do_accept(Sc& c, int fd, uint64_t addr, uint64_t lenp, int flags) {
    int64_t err = 0;
    auto sf = sock_of(c, fd, err);
    if (!sf) return err;
    if (flags & ~(lx::sock_nonblock | lx::sock_cloexec)) return -lx::einval;
    int e = 0;
    auto s = sf->sock()->accept(sf->nonblock(), e);
    if (!s) {
        if (e == lx::eintr) c.t.restartable = true;
        return -e;
    }
    write_unix_addr(s->peer_name(), addr, lenp);
    return c.p.fds.alloc(std::make_shared<SocketFile>(s, flags), (flags & lx::sock_cloexec) != 0);
}

int64_t sys_accept(Sc& c) {
    c.fmt("%d, 0x%llx, 0x%llx", c.fd(0), (unsigned long long)c.a[1], (unsigned long long)c.a[2]);
    return do_accept(c, c.fd(0), c.a[1], c.a[2], 0);
}

int64_t sys_accept4(Sc& c) {
    c.fmt("%d, 0x%llx, 0x%llx, 0x%llx", c.fd(0), (unsigned long long)c.a[1], (unsigned long long)c.a[2], (unsigned long long)c.a[3]);
    return do_accept(c, c.fd(0), c.a[1], c.a[2], (int)c.a[3]);
}

int64_t sys_connect(Sc& c) {
    c.fmt("%d, 0x%llx, %llu", c.fd(0), (unsigned long long)c.a[1], (unsigned long long)c.a[2]);
    int64_t err = 0;
    auto sf = sock_of(c, c.fd(0), err);
    if (!sf) return err;
    std::string name;
    bool abstract;
    int e = parse_unix_addr(c.a[1], c.a[2], name, abstract, true);
    if (e) return e;
    if (!abstract && name.empty()) return -lx::einval;
    c.fmt("%d, {AF_UNIX, \"%s%s\"}, %llu", c.fd(0), abstract ? "@" : "", name.c_str(), (unsigned long long)c.a[2]);
    int le = 0;
    auto target = find_bound(c, name, abstract, le);
    if (!target) return -le;
    int r = sf->sock()->connect(target, sf->nonblock());
    if (r == -lx::eintr) c.t.restartable = true;
    return r;
}

int64_t sys_shutdown(Sc& c) {
    c.fmt("%d, %d", c.fd(0), (int)c.a[1]);
    int64_t err = 0;
    auto sf = sock_of(c, c.fd(0), err);
    if (!sf) return err;
    return sf->sock()->shutdown((int)c.a[1]);
}

int64_t sys_getsockname(Sc& c) {
    c.fmt("%d, 0x%llx, 0x%llx", c.fd(0), (unsigned long long)c.a[1], (unsigned long long)c.a[2]);
    int64_t err = 0;
    auto sf = sock_of(c, c.fd(0), err);
    if (!sf) return err;
    if (!c.a[1] || !c.a[2]) return -lx::efault;
    write_unix_addr(sf->sock()->name(), c.a[1], c.a[2]);
    return 0;
}

int64_t sys_getpeername(Sc& c) {
    c.fmt("%d, 0x%llx, 0x%llx", c.fd(0), (unsigned long long)c.a[1], (unsigned long long)c.a[2]);
    int64_t err = 0;
    auto sf = sock_of(c, c.fd(0), err);
    if (!sf) return err;
    if (!c.a[1] || !c.a[2]) return -lx::efault;
    if (!sf->sock()->has_peer_creds()) return -lx::enotconn;
    write_unix_addr(sf->sock()->peer_name(), c.a[1], c.a[2]);
    return 0;
}

int64_t sys_getsockopt(Sc& c) {
    c.fmt("%d, %d, %d, 0x%llx, 0x%llx", c.fd(0), (int)c.a[1], (int)c.a[2], (unsigned long long)c.a[3], (unsigned long long)c.a[4]);
    int64_t err = 0;
    auto sf = sock_of(c, c.fd(0), err);
    if (!sf) return err;
    if ((int)c.a[1] != lx::sol_socket) return -lx::enoprotoopt;
    auto* lenp = reinterpret_cast<uint32_t*>(c.a[4]);
    if (!c.a[3] || !lenp) return -lx::efault;
    auto& s = *sf->sock();
    auto put_int = [&](int32_t v) {
        const uint32_t n = std::min<uint32_t>(*lenp, 4);
        memcpy(reinterpret_cast<void*>(c.a[3]), &v, n);
        *lenp = n;
        return 0;
    };
    switch ((int)c.a[2]) {
        case lx::so_peercred: {
            if (!s.has_peer_creds()) return -lx::enotconn;
            const SockCreds pc = s.peer_creds();
            lx::ucred u {pc.pid, pc.uid, pc.gid};
            const uint32_t n = std::min<uint32_t>(*lenp, sizeof u);
            memcpy(reinterpret_cast<void*>(c.a[3]), &u, n);
            *lenp = n;
            return 0;
        }
        case lx::so_type: return put_int(s.type());
        case lx::so_error: return put_int(s.take_error());
        case lx::so_acceptconn: return put_int(s.is_listening() ? 1 : 0);
        case lx::so_sndbuf: return put_int((int32_t)s.sndbuf);
        case lx::so_rcvbuf: return put_int((int32_t)s.rcvbuf);
        case lx::so_domain: return put_int(lx::af_unix);
        case lx::so_protocol: return put_int(0);
        case lx::so_passcred: return put_int(s.passcred ? 1 : 0);
        case lx::so_keepalive:
        case lx::so_reuseaddr:
        case lx::so_broadcast:
        case lx::so_oobinline:
        case lx::so_dontroute:
        case lx::so_debug:
        case lx::so_reuseport: return put_int(0);
        case lx::so_rcvlowat:
        case lx::so_sndlowat: return put_int(1);
        case lx::so_rcvtimeo:
        case lx::so_sndtimeo: {
            lx::timeval tv {0, 0};
            const uint32_t n = std::min<uint32_t>(*lenp, sizeof tv);
            memcpy(reinterpret_cast<void*>(c.a[3]), &tv, n);
            *lenp = n;
            return 0;
        }
        case lx::so_linger: {
            int32_t l[2] = {0, 0};
            const uint32_t n = std::min<uint32_t>(*lenp, sizeof l);
            memcpy(reinterpret_cast<void*>(c.a[3]), l, n);
            *lenp = n;
            return 0;
        }
        default: return -lx::enoprotoopt;
    }
}

int64_t sys_setsockopt(Sc& c) {
    c.fmt("%d, %d, %d, 0x%llx, %llu", c.fd(0), (int)c.a[1], (int)c.a[2], (unsigned long long)c.a[3], (unsigned long long)c.a[4]);
    int64_t err = 0;
    auto sf = sock_of(c, c.fd(0), err);
    if (!sf) return err;
    if ((int)c.a[1] != lx::sol_socket) return -lx::enoprotoopt;
    if (!c.a[3]) return -lx::efault;
    if (c.a[4] < 4) return -lx::einval;
    const int32_t v = *reinterpret_cast<const int32_t*>(c.a[3]);
    auto& s = *sf->sock();
    switch ((int)c.a[2]) {
        case lx::so_passcred: s.passcred = v != 0; return 0;
        case lx::so_sndbuf: s.sndbuf = std::clamp<size_t>((size_t)std::max(v, 0) * 2, 4608, 8u << 20); return 0;
        case lx::so_rcvbuf: s.rcvbuf = std::clamp<size_t>((size_t)std::max(v, 0) * 2, 4608, 8u << 20); return 0;
        case lx::so_keepalive:
        case lx::so_reuseaddr:
        case lx::so_reuseport:
        case lx::so_broadcast:
        case lx::so_oobinline:
        case lx::so_linger:
        case lx::so_rcvtimeo:
        case lx::so_sndtimeo:
        case lx::so_rcvlowat:
        case lx::so_sndlowat:
        case lx::so_debug:
        case lx::so_dontroute: return 0;
        default: return -lx::enoprotoopt;
    }
}

// Collects the fds of every SCM_RIGHTS control message. -errno on a bad fd.
int parse_cmsgs(Sc& c, uint64_t control, uint64_t controllen, std::vector<std::shared_ptr<OpenFile>>& fds) {
    if (!control || !controllen) return 0;
    uint64_t off = 0;
    while (off + sizeof(lx::cmsghdr) <= controllen) {
        const auto* h = reinterpret_cast<const lx::cmsghdr*>(control + off);
        if (h->cmsg_len < sizeof(lx::cmsghdr) || off + h->cmsg_len > controllen) return -lx::einval;
        if (h->cmsg_level == lx::sol_socket && h->cmsg_type == lx::scm_rights) {
            const size_t n = (size_t)(h->cmsg_len - sizeof(lx::cmsghdr)) / 4;
            const auto* nums = reinterpret_cast<const int32_t*>(control + off + sizeof(lx::cmsghdr));
            if (fds.size() + n > 253) return -lx::einval;  // SCM_MAX_FD
            for (size_t i = 0; i < n; i++) {
                auto f = fd_get(c, nums[i]);
                if (!f) return -lx::ebadf;
                fds.push_back(std::move(f));
            }
        }
        // SCM_CREDENTIALS: the receiver gets the sender's real identity anyway.
        off += lx::CmsgAlign(h->cmsg_len);
    }
    return 0;
}

// Builds the receiver's control messages: SCM_RIGHTS for the passed files
// (installed into the fd table here) and SCM_CREDENTIALS when asked for.
uint32_t build_cmsgs(Sc& c, lx::msghdr& mh, RecvInfo& info, bool passcred, bool cloexec, uint32_t& flags_out) {
    uint64_t off = 0;
    const uint64_t cap = mh.msg_control ? mh.msg_controllen : 0;
    auto* base = reinterpret_cast<uint8_t*>(mh.msg_control);
    if (!info.fds.empty()) {
        const uint64_t need = lx::CmsgAlign(sizeof(lx::cmsghdr) + 4 * info.fds.size());
        size_t fit = 0;
        if (cap >= off + sizeof(lx::cmsghdr)) {
            fit = std::min(info.fds.size(), (size_t)(cap - off - sizeof(lx::cmsghdr)) / 4);
        }
        if (fit < info.fds.size()) flags_out |= lx::msg_ctrunc;  // the rest is dropped (closed)
        if (fit > 0) {
            auto* h = reinterpret_cast<lx::cmsghdr*>(base + off);
            h->cmsg_len = sizeof(lx::cmsghdr) + 4 * fit;
            h->cmsg_level = lx::sol_socket;
            h->cmsg_type = lx::scm_rights;
            auto* nums = reinterpret_cast<int32_t*>(base + off + sizeof(lx::cmsghdr));
            for (size_t i = 0; i < fit; i++) nums[i] = c.p.fds.alloc(info.fds[i], cloexec);
            off += std::min<uint64_t>(need, cap - off);
        }
    }
    if (passcred && info.has_creds) {
        const uint64_t need = lx::CmsgAlign(sizeof(lx::cmsghdr) + sizeof(lx::ucred));
        if (cap >= off + sizeof(lx::cmsghdr) + sizeof(lx::ucred)) {
            auto* h = reinterpret_cast<lx::cmsghdr*>(base + off);
            h->cmsg_len = sizeof(lx::cmsghdr) + sizeof(lx::ucred);
            h->cmsg_level = lx::sol_socket;
            h->cmsg_type = lx::scm_credentials;
            lx::ucred u {info.creds.pid, info.creds.uid, info.creds.gid};
            memcpy(base + off + sizeof(lx::cmsghdr), &u, sizeof u);
            off += std::min<uint64_t>(need, cap - off);
        } else {
            flags_out |= lx::msg_ctrunc;
        }
    }
    return (uint32_t)off;
}

int64_t do_sendto(Sc& c, int fd, const uint8_t* data, size_t len, int flags, std::vector<std::shared_ptr<OpenFile>> fds,
                  uint64_t addr, uint64_t addrlen) {
    int64_t err = 0;
    auto sf = sock_of(c, fd, err);
    if (!sf) return err;
    std::shared_ptr<UnixSocket> to;
    if (addr && addrlen) {
        std::string name;
        bool abstract;
        int e = parse_unix_addr(addr, addrlen, name, abstract, true);
        if (e) return e;
        if (sf->sock()->type() != lx::sock_dgram) return sf->sock()->is_connected() ? -lx::eisconn : -lx::eopnotsupp;
        int le = 0;
        to = find_bound(c, name, abstract, le);
        if (!to) return -le;
    }
    int64_t r = sf->sock()->send(data, len, std::move(fds), flags, to, nonblocking(*sf, flags));
    if (r == -lx::epipe && !(flags & lx::msg_nosignal)) K().send_signal_thread(c.t, lx::sigpipe, nullptr);
    if (r == -lx::eintr) c.t.restartable = true;
    return r;
}

int64_t sys_sendto(Sc& c) {
    c.fmt("%d, 0x%llx, %llu, 0x%llx, 0x%llx, %llu", c.fd(0), (unsigned long long)c.a[1], (unsigned long long)c.a[2],
          (unsigned long long)c.a[3], (unsigned long long)c.a[4], (unsigned long long)c.a[5]);
    if (!c.a[1] && c.a[2]) return -lx::efault;
    return do_sendto(c, c.fd(0), reinterpret_cast<const uint8_t*>(c.a[1]), (size_t)c.a[2], (int)c.a[3], {}, c.a[4], c.a[5]);
}

int64_t sys_sendmsg(Sc& c) {
    c.fmt("%d, 0x%llx, 0x%llx", c.fd(0), (unsigned long long)c.a[1], (unsigned long long)c.a[2]);
    const auto* mh = reinterpret_cast<const lx::msghdr*>(c.a[1]);
    if (!mh) return -lx::efault;
    if (mh->msg_iovlen > 1024) return -lx::emsgsize;
    std::vector<uint8_t> data;
    const auto* iov = reinterpret_cast<const lx::iovec*>(mh->msg_iov);
    for (uint64_t i = 0; i < mh->msg_iovlen; i++) {
        if (!iov) return -lx::efault;
        const auto* p = reinterpret_cast<const uint8_t*>(iov[i].iov_base);
        if (iov[i].iov_len && !p) return -lx::efault;
        data.insert(data.end(), p, p + iov[i].iov_len);
    }
    std::vector<std::shared_ptr<OpenFile>> fds;
    int e = parse_cmsgs(c, mh->msg_control, mh->msg_controllen, fds);
    if (e) return e;
    if (!fds.empty()) c.fmt("%d, {iov=%llu bytes, fds=%zu}, 0x%llx", c.fd(0), (unsigned long long)data.size(), fds.size(), (unsigned long long)c.a[2]);
    return do_sendto(c, c.fd(0), data.data(), data.size(), (int)c.a[2], std::move(fds), mh->msg_name, mh->msg_namelen);
}

int64_t sys_recvfrom(Sc& c) {
    c.fmt("%d, 0x%llx, %llu, 0x%llx, 0x%llx, 0x%llx", c.fd(0), (unsigned long long)c.a[1], (unsigned long long)c.a[2],
          (unsigned long long)c.a[3], (unsigned long long)c.a[4], (unsigned long long)c.a[5]);
    int64_t err = 0;
    auto sf = sock_of(c, c.fd(0), err);
    if (!sf) return err;
    if (!c.a[1] && c.a[2]) return -lx::efault;
    const int flags = (int)c.a[3];
    RecvInfo info;
    int64_t r = sf->sock()->recv(reinterpret_cast<uint8_t*>(c.a[1]), (size_t)c.a[2], flags, nonblocking(*sf, flags), &info);
    if (r == -lx::eintr) c.t.restartable = true;
    if (r >= 0 && c.a[4] && c.a[5]) write_unix_addr(info.from, c.a[4], c.a[5]);
    return r;
}

int64_t sys_recvmsg(Sc& c) {
    c.fmt("%d, 0x%llx, 0x%llx", c.fd(0), (unsigned long long)c.a[1], (unsigned long long)c.a[2]);
    int64_t err = 0;
    auto sf = sock_of(c, c.fd(0), err);
    if (!sf) return err;
    auto* mh = reinterpret_cast<lx::msghdr*>(c.a[1]);
    if (!mh) return -lx::efault;
    if (mh->msg_iovlen > 1024) return -lx::emsgsize;
    const int flags = (int)c.a[2];
    const auto* iov = reinterpret_cast<const lx::iovec*>(mh->msg_iov);
    size_t total = 0;
    for (uint64_t i = 0; i < mh->msg_iovlen; i++) {
        if (!iov) return -lx::efault;
        total += iov[i].iov_len;
    }
    std::vector<uint8_t> buf(total);
    RecvInfo info;
    int64_t r = sf->sock()->recv(buf.data(), total, flags, nonblocking(*sf, flags), &info);
    if (r == -lx::eintr) c.t.restartable = true;
    if (r < 0) return r;
    // scatter
    size_t done = 0;
    const size_t got = std::min<size_t>((size_t)r, total);
    for (uint64_t i = 0; i < mh->msg_iovlen && done < got; i++) {
        const size_t n = std::min<size_t>(iov[i].iov_len, got - done);
        memcpy(reinterpret_cast<void*>(iov[i].iov_base), buf.data() + done, n);
        done += n;
    }
    uint32_t out_flags = 0;
    if (info.truncated) out_flags |= lx::msg_trunc;
    mh->msg_controllen = build_cmsgs(c, *mh, info, sf->sock()->passcred, (flags & lx::msg_cmsg_cloexec) != 0, out_flags);
    if (mh->msg_name && mh->msg_namelen) {
        uint32_t len = mh->msg_namelen;
        write_unix_addr(info.from, mh->msg_name, reinterpret_cast<uint64_t>(&len));
        mh->msg_namelen = info.from.empty() ? 0 : len;
    } else {
        mh->msg_namelen = 0;
    }
    mh->msg_flags = out_flags;
    if (!info.fds.empty()) c.fmt("%d, {iov=%zu bytes, fds=%zu}, 0x%llx", c.fd(0), got, info.fds.size(), (unsigned long long)c.a[2]);
    return r;
}

// ---- interval timers ---------------------------------------------------------------------

int64_t tv_to_ns(const lx::timeval& tv) {
    return tv.tv_sec * 1000000000LL + tv.tv_usec * 1000LL;
}

lx::timeval ns_to_tv(int64_t ns) {
    if (ns <= 0) return {0, 0};
    return {ns / 1000000000LL, (ns % 1000000000LL) / 1000};
}

int64_t sys_setitimer(Sc& c) {
    c.fmt("%d, 0x%llx, 0x%llx", (int)c.a[0], (unsigned long long)c.a[1], (unsigned long long)c.a[2]);
    const int which = (int)c.a[0];
    if (which < lx::itimer_real || which > lx::itimer_prof) return -lx::einval;
    const auto* nv = reinterpret_cast<const lx::itimerval*>(c.a[1]);
    auto* ov = reinterpret_cast<lx::itimerval*>(c.a[2]);
    if (which != lx::itimer_real) {  // CPU-time timers: accepted, never fire (no per-guest CPU accounting)
        if (ov) memset(ov, 0, sizeof *ov);
        return 0;
    }
    int64_t old_value = 0, old_interval = 0;
    if (nv) {
        if (nv->it_value.tv_usec < 0 || nv->it_value.tv_usec >= 1000000 || nv->it_interval.tv_usec < 0 ||
            nv->it_interval.tv_usec >= 1000000) {
            return -lx::einval;
        }
        K().set_itimer(c.p, tv_to_ns(nv->it_value), tv_to_ns(nv->it_interval), &old_value, &old_interval);
    } else {
        K().get_itimer(c.p, &old_value, &old_interval);
    }
    if (ov) {
        ov->it_value = ns_to_tv(old_value);
        ov->it_interval = ns_to_tv(old_interval);
    }
    return 0;
}

int64_t sys_getitimer(Sc& c) {
    c.fmt("%d, 0x%llx", (int)c.a[0], (unsigned long long)c.a[1]);
    auto* ov = reinterpret_cast<lx::itimerval*>(c.a[1]);
    if (!ov) return -lx::efault;
    int64_t value = 0, interval = 0;
    if ((int)c.a[0] == lx::itimer_real) K().get_itimer(c.p, &value, &interval);
    ov->it_value = ns_to_tv(value);
    ov->it_interval = ns_to_tv(interval);
    return 0;
}

int64_t sys_alarm(Sc& c) {
    c.fmt("%u", (unsigned)c.a[0]);
    int64_t old_value = 0, old_interval = 0;
    K().set_itimer(c.p, (int64_t)(uint32_t)c.a[0] * 1000000000LL, 0, &old_value, &old_interval);
    return (old_value + 999999999LL) / 1000000000LL;
}

int64_t sys_enosys_io(Sc& c) {
    c.fmt("0x%llx, 0x%llx, 0x%llx", (unsigned long long)c.a[0], (unsigned long long)c.a[1], (unsigned long long)c.a[2]);
    return -lx::enosys;
}

}  // namespace

void register_io_syscalls(SetFn set) {
    set(7, sys_poll); set(271, sys_ppoll); set(23, sys_select); set(270, sys_pselect6);
    set(213, sys_epoll_create); set(291, sys_epoll_create1); set(233, sys_epoll_ctl); set(232, sys_epoll_wait);
    set(281, sys_epoll_pwait); set(441, sys_epoll_pwait2);
    set(284, sys_eventfd); set(290, sys_eventfd2);
    set(41, sys_socket); set(53, sys_socketpair); set(49, sys_bind); set(50, sys_listen); set(43, sys_accept);
    set(288, sys_accept4); set(42, sys_connect); set(48, sys_shutdown); set(51, sys_getsockname);
    set(52, sys_getpeername); set(55, sys_getsockopt); set(54, sys_setsockopt); set(44, sys_sendto);
    set(46, sys_sendmsg); set(45, sys_recvfrom); set(47, sys_recvmsg);
    set(299, sys_enosys_io /*recvmmsg*/); set(307, sys_enosys_io /*sendmmsg*/);
    set(38, sys_setitimer); set(36, sys_getitimer); set(37, sys_alarm);
}

}  // namespace rlk
