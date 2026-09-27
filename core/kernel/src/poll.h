// poll.h — readiness polling shared by poll(2)/ppoll, select(2)/pselect6 and
// epoll(7), plus eventfd(2). A file's poll() reports its readiness bits and,
// when given a table, registers the caller's Waiter on the wait queues whose
// wakeups can change that readiness (a pipe's reader queue, a socket's
// receive queue, ...). The poller then sleeps on its Waiter; any of those
// queues (or a signal) wakes it and it polls again. Registration is redone
// on every round, like Linux's poll_table.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "fds.h"
#include "waitq.h"

namespace rlk {

struct PollTable {
    explicit PollTable(Waiter* w) : waiter(w) {}
    ~PollTable() { clear(); }
    PollTable(const PollTable&) = delete;
    PollTable& operator=(const PollTable&) = delete;
    void add(WaitQueue& q) {
        q.add(waiter);
        queues.push_back(&q);
    }
    void clear() {
        for (auto* q : queues) q->remove(waiter);
        queues.clear();
    }
    Waiter* waiter;
    std::vector<WaitQueue*> queues;
};

struct PollEntry {
    std::shared_ptr<OpenFile> file;  // nullptr = bad fd (POLLNVAL)
    unsigned events = 0;             // requested lx::poll* bits
    unsigned revents = 0;            // result
};

// Polls the entries until one is ready, the monotonic deadline (ns; 0 = no
// deadline) passes, or a signal interrupts the caller. nowait: one pass.
// Returns the number of ready entries, 0 on timeout, -EINTR.
int64_t DoPoll(std::vector<PollEntry>& entries, int64_t deadline_ns, bool nowait);

// eventfd(2): a 64-bit counter.
class EventFdFile final : public OpenFile {
public:
    EventFdFile(uint64_t initial, int flags);  // lx::efd_*
    int64_t read(void* buf, size_t len) override;
    int64_t write(const void* buf, size_t len) override;
    int fstat(lx::stat& st) override;
    unsigned poll(PollTable* pt) override;

private:
    std::mutex m_;
    uint64_t count_;
    bool semaphore_;
    WaitQueue rq_, wq_;
    uint64_t ino_;
};

// epoll(7): an interest list polled level-triggered; EPOLLET entries report
// on the rising edge of their readiness, EPOLLONESHOT entries once until
// re-armed with EPOLL_CTL_MOD.
class EpollFile final : public OpenFile {
public:
    EpollFile();
    // op: lx::epoll_ctl_*; f: the file behind fd (null for DEL). -errno.
    int ctl(int op, int fd, const std::shared_ptr<OpenFile>& f, uint32_t events, uint64_t data);
    // Up to `max` events into out. Returns the count, 0 on timeout, -EINTR.
    int64_t wait(lx::epoll_event* out, int max, int64_t deadline_ns, bool nowait);
    int fstat(lx::stat& st) override;
    unsigned poll(PollTable* pt) override;  // readable when a member is ready
    size_t size();

private:
    struct Entry {
        std::weak_ptr<OpenFile> file;
        OpenFile* key = nullptr;   // identity check when the fd number is reused
        uint32_t events = 0;
        uint64_t data = 0;
        bool disabled = false;     // EPOLLONESHOT fired
        bool fresh = true;         // just added/modified: report the current state once, edge-triggered or not
        uint64_t last_wakes = 0;   // EPOLLET: the file's wait-queue wake counters at the last scan
    };
    // One pass over the interest list (m_ held): fills out, returns the count.
    int scan_locked(PollTable* pt, lx::epoll_event* out, int max);
    std::mutex m_;
    std::map<int, Entry> entries_;
    uint64_t ino_;
};

}  // namespace rlk
