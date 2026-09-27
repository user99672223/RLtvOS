// futex.cpp — see futex.h.
#include "futex.h"

#include <atomic>

#include "linux_abi.h"

namespace rlk {

FutexTable& FutexTable::get() {
    static FutexTable* t = new FutexTable();
    return *t;
}

WaitQueue& FutexTable::bucket(uint64_t key) {
    // mu_ held
    auto it = buckets_.find(key);
    if (it == buckets_.end()) it = buckets_.emplace(key, std::make_unique<WaitQueue>()).first;
    return *it->second;
}

int FutexTable::wait(uint32_t* uaddr, uint32_t val, int64_t deadline_ns, uint32_t bitset) {
    if (bitset == 0) return -lx::einval;
    Waiter local;
    Waiter* w = CurrentWaiter() ? CurrentWaiter() : &local;
    const uint64_t key = reinterpret_cast<uint64_t>(uaddr);
    w->bitset = bitset;
    w->key = key;
    WaitQueue* q;
    uint64_t gen;
    {
        std::lock_guard<std::mutex> lk(mu_);
        // The value check and the enqueue are one step under the table lock:
        // a waker that changes the word and calls wake() afterwards takes
        // the same lock and sees us queued.
        if (__atomic_load_n(uaddr, __ATOMIC_SEQ_CST) != val) return -lx::eagain;
        q = &bucket(key);
        q->add(w);
        gen = w->prepare();
    }
    auto r = w->wait(gen, deadline_ns);
    {
        std::lock_guard<std::mutex> lk(mu_);
        // requeue may have moved us: remove from wherever we are now
        bucket(w->key).remove(w);
        if (w->key != key) bucket(key).remove(w);
    }
    w->bitset = ~0u;
    switch (r) {
        case Waiter::Result::Ready: return 0;
        case Waiter::Result::Timeout: return -lx::etimedout;
        case Waiter::Result::Interrupted: return -lx::eintr;
    }
    return 0;
}

int FutexTable::wake(uint32_t* uaddr, int max, uint32_t bitset) {
    if (bitset == 0) return -lx::einval;
    if (max <= 0) return 0;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = buckets_.find(reinterpret_cast<uint64_t>(uaddr));
    if (it == buckets_.end()) return 0;
    return (int)it->second->wake((size_t)max, bitset);
}

int FutexTable::requeue(uint32_t* uaddr, int wake_max, int requeue_max, uint32_t* uaddr2, bool cmp, uint32_t val) {
    std::lock_guard<std::mutex> lk(mu_);
    if (cmp && __atomic_load_n(uaddr, __ATOMIC_SEQ_CST) != val) return -lx::eagain;
    auto it = buckets_.find(reinterpret_cast<uint64_t>(uaddr));
    if (it == buckets_.end()) return 0;
    int n = (int)it->second->wake(wake_max < 0 ? 0 : (size_t)wake_max);
    if (requeue_max > 0) {
        WaitQueue& to = bucket(reinterpret_cast<uint64_t>(uaddr2));
        n += (int)it->second->requeue((size_t)requeue_max, to, reinterpret_cast<uint64_t>(uaddr2));
    }
    return n;
}

int FutexTable::wake_op(uint32_t* uaddr, int max1, uint32_t* uaddr2, int max2, uint32_t encoded) {
    // FUTEX_OP(op, oparg, cmp, cmparg): op = (encoded >> 28) & 0xf, cmp = (encoded >> 24) & 0xf,
    // oparg = (encoded >> 12) & 0xfff, cmparg = encoded & 0xfff; op bit 3 = shift oparg.
    const uint32_t op = (encoded >> 28) & 0xf, cmp = (encoded >> 24) & 0xf;
    uint32_t oparg = (encoded >> 12) & 0xfff;
    const uint32_t cmparg = encoded & 0xfff;
    if (op & 8) oparg = 1u << oparg;
    uint32_t old;
    switch (op & 7) {
        case 0: old = __atomic_exchange_n(uaddr2, oparg, __ATOMIC_SEQ_CST); break;        // SET
        case 1: old = __atomic_fetch_add(uaddr2, oparg, __ATOMIC_SEQ_CST); break;         // ADD
        case 2: old = __atomic_fetch_or(uaddr2, oparg, __ATOMIC_SEQ_CST); break;          // OR
        case 3: old = __atomic_fetch_and(uaddr2, ~oparg, __ATOMIC_SEQ_CST); break;        // ANDN
        case 4: old = __atomic_fetch_xor(uaddr2, oparg, __ATOMIC_SEQ_CST); break;         // XOR
        default: return -lx::enosys;
    }
    bool cond;
    switch (cmp) {
        case 0: cond = old == cmparg; break;
        case 1: cond = old != cmparg; break;
        case 2: cond = old < cmparg; break;
        case 3: cond = old <= cmparg; break;
        case 4: cond = old > cmparg; break;
        case 5: cond = old >= cmparg; break;
        default: return -lx::enosys;
    }
    int n = wake(uaddr, max1, ~0u);
    if (cond) n += wake(uaddr2, max2, ~0u);
    return n;
}

}  // namespace rlk
