// timers.cpp — ITIMER_REAL / alarm(2): one kernel thread sleeps until the
// earliest deadline and posts SIGALRM to the owning process (delivered at
// that process's next syscall boundary, like every guest signal).
#include <pthread.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "process.h"

namespace rlk {

void Kernel::set_itimer(GuestProcess& p, int64_t value_ns, int64_t interval_ns, int64_t* old_value_ns,
                        int64_t* old_interval_ns) {
    std::lock_guard<std::mutex> lk(timer_mu_);
    const int64_t now = MonotonicNs();
    if (old_value_ns) *old_value_ns = p.itimer_deadline_ns ? std::max<int64_t>(0, p.itimer_deadline_ns - now) : 0;
    if (old_interval_ns) *old_interval_ns = p.itimer_deadline_ns ? p.itimer_interval_ns : 0;
    p.itimer_deadline_ns = value_ns > 0 ? now + value_ns : 0;
    p.itimer_interval_ns = value_ns > 0 ? interval_ns : 0;
    if (p.itimer_deadline_ns && !timer_thread_started_) {
        timer_thread_started_ = true;
        std::thread([this] { timer_thread_main(); }).detach();
    }
    timer_cv_.notify_all();
}

void Kernel::get_itimer(GuestProcess& p, int64_t* value_ns, int64_t* interval_ns) {
    std::lock_guard<std::mutex> lk(timer_mu_);
    const int64_t now = MonotonicNs();
    if (value_ns) *value_ns = p.itimer_deadline_ns ? std::max<int64_t>(0, p.itimer_deadline_ns - now) : 0;
    if (interval_ns) *interval_ns = p.itimer_deadline_ns ? p.itimer_interval_ns : 0;
}

void Kernel::timer_thread_main() {
#ifdef __APPLE__
    pthread_setname_np("rl.kernel.timers");
#else
    pthread_setname_np(pthread_self(), "rl.kernel.timers");
#endif
    std::unique_lock<std::mutex> lk(timer_mu_);
    for (;;) {
        int64_t next = 0;
        std::vector<GuestProcess*> due;
        const int64_t now = MonotonicNs();
        {
            std::lock_guard<std::mutex> pl(mu);  // lock order: timer_mu_ → mu (set_itimer never holds mu)
            for (auto& [pid, proc] : procs) {
                if (!proc->itimer_deadline_ns) continue;
                if (proc->state.load() != (int)ProcState::Running) {
                    proc->itimer_deadline_ns = 0;
                    continue;
                }
                if (proc->itimer_deadline_ns <= now) {
                    due.push_back(proc.get());
                    if (proc->itimer_interval_ns > 0) {
                        do {
                            proc->itimer_deadline_ns += proc->itimer_interval_ns;
                        } while (proc->itimer_deadline_ns <= now);  // missed periods collapse into one signal
                    } else {
                        proc->itimer_deadline_ns = 0;
                    }
                }
                if (proc->itimer_deadline_ns && (!next || proc->itimer_deadline_ns < next)) next = proc->itimer_deadline_ns;
            }
        }
        lk.unlock();
        for (auto* proc : due) {
            lx::siginfo si {};
            si.si_signo = lx::sigalrm;
            si.si_code = lx::si_kernel;
            send_signal(*proc, lx::sigalrm, &si);
        }
        lk.lock();
        if (next) {
            timer_cv_.wait_until(lk, std::chrono::steady_clock::time_point(std::chrono::nanoseconds(next)));
        } else {
            timer_cv_.wait(lk);
        }
    }
}

// ---- shared file mappings (write-back model) ----------------------------------------

void Kernel::add_shared_map(GuestProcess& p, uint64_t start, uint64_t len, std::shared_ptr<TmpNode> node, uint64_t off) {
    writeback_shared(p, start, len, true);  // a mapping replaced by MAP_FIXED flushes first
    std::lock_guard<std::mutex> lk(p.maps_mu);
    p.shared_maps.push_back({start, len, std::move(node), off});
}

void Kernel::writeback_shared(GuestProcess& p, uint64_t addr, uint64_t len, bool drop) {
    std::lock_guard<std::mutex> lk(p.maps_mu);
    if (p.shared_maps.empty() || !p.mm) return;
    const uint64_t end = len ? addr + len : UINT64_MAX;
    std::vector<SharedMap> keep;
    for (auto& m : p.shared_maps) {
        const uint64_t ms = m.start, me = m.start + m.len;
        if (me <= addr || ms >= end) {
            keep.push_back(m);
            continue;
        }
        const uint64_t s = std::max(ms, addr), e = std::min(me, end);
        {
            std::lock_guard<std::mutex> dl(m.node->data->mu);
            auto& bytes = m.node->data->bytes;
            const uint64_t foff = m.off + (s - ms);
            if (foff < bytes.size()) {
                const uint64_t n = std::min<uint64_t>(e - s, bytes.size() - foff);
                for (uint64_t pg = s; pg < s + n; pg += lx::page) {  // only pages the guest can read
                    const Vma v = p.mm->find(pg);
                    if (v.end == 0 || !(v.prot_at(pg) & lx::prot_read)) continue;
                    const uint64_t chunk = std::min<uint64_t>(lx::page - (pg & (lx::page - 1)), s + n - pg);
                    memcpy(bytes.data() + foff + (pg - s), reinterpret_cast<const void*>(pg), (size_t)chunk);
                }
                m.node->mtime = m.node->ctime = Overlay::now_sec();
            }
        }
        if (!drop) {
            keep.push_back(m);
            continue;
        }
        if (s > ms) keep.push_back({ms, s - ms, m.node, m.off});                          // left remainder
        if (e < me) keep.push_back({e, me - e, m.node, m.off + (e - ms)});                 // right remainder
    }
    p.shared_maps.swap(keep);
}

}  // namespace rlk
