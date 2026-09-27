// futex.h — futex(2) on host condition variables. One table for the whole
// address space (guest address == host address, so private and shared
// futexes are the same thing here): a bucket of Waiters per word.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "waitq.h"

namespace rlk {

class FutexTable {
public:
    // FUTEX_WAIT / FUTEX_WAIT_BITSET. deadline_ns: monotonic ns, 0 = none.
    // Returns 0, -EAGAIN (*uaddr != val), -ETIMEDOUT, -EINTR (signal).
    int wait(uint32_t* uaddr, uint32_t val, int64_t deadline_ns, uint32_t bitset);
    // FUTEX_WAKE / FUTEX_WAKE_BITSET: number of waiters woken.
    int wake(uint32_t* uaddr, int max, uint32_t bitset);
    // FUTEX_REQUEUE / FUTEX_CMP_REQUEUE: wake up to `wake_max`, move up to
    // `requeue_max` to uaddr2. cmp: check *uaddr == val first (-EAGAIN).
    int requeue(uint32_t* uaddr, int wake_max, int requeue_max, uint32_t* uaddr2, bool cmp, uint32_t val);
    // FUTEX_WAKE_OP: perform op on *uaddr2, wake uaddr, conditionally uaddr2.
    int wake_op(uint32_t* uaddr, int max1, uint32_t* uaddr2, int max2, uint32_t op);

    static FutexTable& get();

private:
    WaitQueue& bucket(uint64_t key);
    std::mutex mu_;
    std::unordered_map<uint64_t, std::unique_ptr<WaitQueue>> buckets_;
};

}  // namespace rlk
