// waitq.h — how a guest thread blocks inside the kernel. A blocking syscall
// parks the calling host thread on that guest thread's Waiter; the object it
// waits for (pipe, futex word, child, timer) keeps the Waiter in a WaitQueue
// and notifies it. A signal sent to the thread notifies the same Waiter, so
// every blocking syscall can return -EINTR (the caller restarts it per
// SA_RESTART). Lost wakeups are avoided with the prepare/wait pattern: the
// caller registers, takes the wakeup generation, checks its condition, then
// sleeps until the generation changes.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace rlk {

struct Waiter {
    enum class Result { Ready, Timeout, Interrupted };

    std::mutex mu;
    std::condition_variable cv;
    uint64_t wakeups = 0;      // bumped by notify()
    uint32_t bitset = ~0u;     // futex: which wake bits this waiter accepts
    uint64_t key = 0;          // futex: the word it sleeps on (requeue moves it)
    // Set by the owner: true when a signal wants delivering to this thread.
    bool (*interrupted)(void* arg) = nullptr;
    void* interrupted_arg = nullptr;

    uint64_t prepare() {
        std::lock_guard<std::mutex> lk(mu);
        return wakeups;
    }
    void notify() {
        {
            std::lock_guard<std::mutex> lk(mu);
            ++wakeups;
        }
        cv.notify_all();
    }
    // Sleeps until notify() moved the generation past `gen`, the monotonic
    // deadline (ns, 0 = none) passes, or interrupted() says so.
    Result wait(uint64_t gen, int64_t deadline_ns) {
        std::unique_lock<std::mutex> lk(mu);
        while (wakeups == gen) {
            if (interrupted && interrupted(interrupted_arg)) return Result::Interrupted;
            if (deadline_ns) {
                const auto tp = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(deadline_ns));
                if (std::chrono::steady_clock::now() >= tp) return Result::Timeout;
                cv.wait_until(lk, tp);
            } else {
                cv.wait(lk);
            }
        }
        return Result::Ready;
    }
};

inline int64_t MonotonicNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

class WaitQueue {
public:
    void add(Waiter* w) {
        std::lock_guard<std::mutex> lk(mu_);
        waiters_.push_back(w);
    }
    void remove(Waiter* w) {
        std::lock_guard<std::mutex> lk(mu_);
        for (size_t i = 0; i < waiters_.size(); i++) {
            if (waiters_[i] == w) {
                waiters_.erase(waiters_.begin() + (long)i);
                return;
            }
        }
    }
    // Wakes up to `max` waiters whose bitset intersects `bits`. Returns the count.
    size_t wake(size_t max = SIZE_MAX, uint32_t bits = ~0u) {
        std::vector<Waiter*> hit;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (Waiter* w : waiters_) {
                if (hit.size() >= max) break;
                if (w->bitset & bits) hit.push_back(w);
            }
        }
        for (Waiter* w : hit) w->notify();
        return hit.size();
    }
    // Moves up to `max` waiters (all bitsets) to `to`, giving them `new_key`.
    size_t requeue(size_t max, WaitQueue& to, uint64_t new_key) {
        std::vector<Waiter*> moved;
        {
            std::lock_guard<std::mutex> lk(mu_);
            while (!waiters_.empty() && moved.size() < max) {
                moved.push_back(waiters_.front());
                waiters_.erase(waiters_.begin());
            }
        }
        for (Waiter* w : moved) {
            w->key = new_key;
            to.add(w);
        }
        return moved.size();
    }
    size_t size() {
        std::lock_guard<std::mutex> lk(mu_);
        return waiters_.size();
    }

private:
    std::mutex mu_;
    std::vector<Waiter*> waiters_;
};

// The Waiter of the guest thread running on this host thread (nullptr on app
// threads and in tests: callers then block on a private, uninterruptible one).
Waiter* CurrentWaiter();
void SetCurrentWaiter(Waiter* w);

}  // namespace rlk
