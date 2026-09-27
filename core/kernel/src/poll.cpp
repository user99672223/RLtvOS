// poll.cpp — see poll.h.
#include "poll.h"

#include <atomic>
#include <cstring>

#include "linux_abi.h"

namespace rlk {

namespace {
std::atomic<uint64_t> g_anon_ino {0x70000};
}

// ---- DoPoll ---------------------------------------------------------------------------

int64_t DoPoll(std::vector<PollEntry>& entries, int64_t deadline_ns, bool nowait) {
    Waiter local;
    Waiter* w = CurrentWaiter() ? CurrentWaiter() : &local;
    for (;;) {
        const uint64_t gen = w->prepare();
        PollTable pt(w);
        int64_t count = 0;
        for (auto& e : entries) {
            if (!e.file) {
                e.revents = lx::pollnval;
                count++;
                continue;
            }
            const unsigned m = e.file->poll(nowait ? nullptr : &pt);
            e.revents = m & (e.events | lx::pollerr | lx::pollhup | lx::pollnval);
            if (e.revents) count++;
        }
        if (count > 0 || nowait) return count;
        if (deadline_ns && MonotonicNs() >= deadline_ns) return 0;
        const auto r = w->wait(gen, deadline_ns);
        pt.clear();
        if (r == Waiter::Result::Interrupted) return -lx::eintr;
        if (r == Waiter::Result::Timeout) nowait = true;  // one last pass
    }
}

// ---- eventfd -------------------------------------------------------------------------

EventFdFile::EventFdFile(uint64_t initial, int flags)
    : count_(initial), semaphore_((flags & lx::efd_semaphore) != 0), ino_(g_anon_ino.fetch_add(1)) {
    path = "anon_inode:[eventfd]";
    oflags = lx::o_rdwr | ((flags & lx::efd_nonblock) ? lx::o_nonblock : 0);
}

int64_t EventFdFile::read(void* buf, size_t len) {
    if (len < 8) return -lx::einval;
    Waiter local;
    Waiter* w = CurrentWaiter() ? CurrentWaiter() : &local;
    for (;;) {
        rq_.add(w);
        const uint64_t gen = w->prepare();
        {
            std::lock_guard<std::mutex> lk(m_);
            if (count_ > 0) {
                const uint64_t v = semaphore_ ? 1 : count_;
                count_ -= v;
                memcpy(buf, &v, 8);
                rq_.remove(w);
                wq_.wake();
                return 8;
            }
            if (oflags & lx::o_nonblock) {
                rq_.remove(w);
                return -lx::eagain;
            }
        }
        const auto r = w->wait(gen, 0);
        rq_.remove(w);
        if (r == Waiter::Result::Interrupted) return -lx::eintr;
    }
}

int64_t EventFdFile::write(const void* buf, size_t len) {
    if (len < 8) return -lx::einval;
    uint64_t v;
    memcpy(&v, buf, 8);
    if (v == ~0ull) return -lx::einval;
    Waiter local;
    Waiter* w = CurrentWaiter() ? CurrentWaiter() : &local;
    for (;;) {
        wq_.add(w);
        const uint64_t gen = w->prepare();
        {
            std::lock_guard<std::mutex> lk(m_);
            if (count_ + v < ~0ull - 1 || v == 0) {
                count_ += v;
                wq_.remove(w);
                rq_.wake();
                return 8;
            }
            if (oflags & lx::o_nonblock) {
                wq_.remove(w);
                return -lx::eagain;
            }
        }
        const auto r = w->wait(gen, 0);
        wq_.remove(w);
        if (r == Waiter::Result::Interrupted) return -lx::eintr;
    }
}

int EventFdFile::fstat(lx::stat& st) {
    FillStat(st, 0600, 0, 0, ino_);  // anon inode: permission bits only, like Linux
    return 0;
}

unsigned EventFdFile::poll(PollTable* pt) {
    if (pt) {
        pt->add(rq_);
        pt->add(wq_);
    }
    std::lock_guard<std::mutex> lk(m_);
    unsigned m = 0;
    if (count_ > 0) m |= lx::pollin;
    if (count_ < ~0ull - 1) m |= lx::pollout;
    return m;
}

// ---- epoll ---------------------------------------------------------------------------

EpollFile::EpollFile() : ino_(g_anon_ino.fetch_add(1)) {
    path = "anon_inode:[eventpoll]";
    oflags = lx::o_rdwr;
}

int EpollFile::ctl(int op, int fd, const std::shared_ptr<OpenFile>& f, uint32_t events, uint64_t data) {
    std::lock_guard<std::mutex> lk(m_);
    auto it = entries_.find(fd);
    const bool present = it != entries_.end() && it->second.key == f.get() && !it->second.file.expired();
    switch (op) {
        case lx::epoll_ctl_add: {
            if (!f) return -lx::ebadf;
            if (f.get() == this) return -lx::einval;
            if (present) return -lx::eexist;
            Entry e;
            e.file = f;
            e.key = f.get();
            e.events = events;
            e.data = data;
            entries_[fd] = e;
            return 0;
        }
        case lx::epoll_ctl_mod:
            if (!f) return -lx::ebadf;
            if (!present) return -lx::enoent;
            it->second.events = events;
            it->second.data = data;
            it->second.disabled = false;
            it->second.fresh = true;
            return 0;
        case lx::epoll_ctl_del:
            if (it == entries_.end()) return -lx::enoent;
            entries_.erase(it);
            return 0;
        default: return -lx::einval;
    }
}

// Edge-triggered entries (Xorg registers every client this way): Linux puts the
// item on the ready list at every wake-up of the file's wait queue, so an edge
// is "woken since the last scan", never "the mask changed" — result 006's xeyes
// hang was a mask comparison that missed a drain-then-refill between two scans.
int EpollFile::scan_locked(PollTable* pt, lx::epoll_event* out, int max) {
    // Without a caller's table (nowait) a scratch one still records each file's
    // wait queues, whose wake counters drive the edge-triggered entries.
    Waiter scratch;
    PollTable spt(&scratch);
    PollTable* use = pt ? pt : &spt;
    int n = 0;
    for (auto it = entries_.begin(); it != entries_.end();) {
        auto f = it->second.file.lock();
        if (!f) {  // the file was closed everywhere: Linux drops the registration
            it = entries_.erase(it);
            continue;
        }
        Entry& e = it->second;
        ++it;
        if (e.disabled) continue;
        const uint32_t want = e.events & ~(lx::epollet | lx::epolloneshot | lx::epollexclusive | lx::epollwakeup);
        const uint32_t care = want | lx::epollerr | lx::epollhup;
        // Register the waiter first (the caller may sleep on it), then read the
        // wake counters, then the mask: a wake-up after the counter read either
        // shows in the mask now or bumps a counter for the next scan, and since
        // the waiter is already queued it also ends the caller's sleep.
        const size_t q0 = use->queues.size();
        uint32_t m = f->poll(use) & care;
        if (e.events & lx::epollet) {
            uint64_t wakes = 0;
            for (size_t i = q0; i < use->queues.size(); i++) wakes += use->queues[i]->wakes();
            m = f->poll(nullptr) & care;
            const bool pending = e.fresh || wakes != e.last_wakes;
            e.last_wakes = wakes;
            e.fresh = false;
            if (!pending) m = 0;
        }
        if (!m) continue;
        if (out && n < max) {
            out[n].events = m;
            out[n].data = e.data;
        }
        n++;
        if (e.events & lx::epolloneshot) e.disabled = true;
        if (n >= max) break;
    }
    return n;
}

int64_t EpollFile::wait(lx::epoll_event* out, int max, int64_t deadline_ns, bool nowait) {
    if (max <= 0) return -lx::einval;
    Waiter local;
    Waiter* w = CurrentWaiter() ? CurrentWaiter() : &local;
    for (;;) {
        const uint64_t gen = w->prepare();
        PollTable pt(w);
        int n;
        {
            std::lock_guard<std::mutex> lk(m_);
            n = scan_locked(nowait ? nullptr : &pt, out, max);
        }
        if (n > 0 || nowait) return n;
        if (deadline_ns && MonotonicNs() >= deadline_ns) return 0;
        const auto r = w->wait(gen, deadline_ns);
        pt.clear();
        if (r == Waiter::Result::Interrupted) return -lx::eintr;
        if (r == Waiter::Result::Timeout) nowait = true;
    }
}

unsigned EpollFile::poll(PollTable* pt) {
    std::lock_guard<std::mutex> lk(m_);
    // The set as one file: readable when a scan would report something (edge
    // state left untouched; the caller's table is registered with every member).
    Waiter scratch;
    PollTable spt(&scratch);
    PollTable* use = pt ? pt : &spt;
    for (auto& [fd, e] : entries_) {
        auto f = e.file.lock();
        if (!f || e.disabled) continue;
        const uint32_t want = e.events & ~(lx::epollet | lx::epolloneshot | lx::epollexclusive | lx::epollwakeup);
        const uint32_t care = want | lx::epollerr | lx::epollhup;
        const size_t q0 = use->queues.size();
        uint32_t m = f->poll(use) & care;
        if (e.events & lx::epollet) {
            uint64_t wakes = 0;
            for (size_t i = q0; i < use->queues.size(); i++) wakes += use->queues[i]->wakes();
            m = f->poll(nullptr) & care;
            if (!(e.fresh || wakes != e.last_wakes)) m = 0;
        }
        if (m) return lx::pollin;
    }
    return 0;
}

int EpollFile::fstat(lx::stat& st) {
    FillStat(st, 0600, 0, 0, ino_);
    return 0;
}

size_t EpollFile::size() {
    std::lock_guard<std::mutex> lk(m_);
    return entries_.size();
}

}  // namespace rlk
