// locks.cpp — see locks.h.
#include "locks.h"

#include <algorithm>

namespace rlk {

LockTable& LockTable::get() {
    static LockTable t;
    return t;
}

const FileLock* LockTable::conflict_locked(const Owned& o, uint64_t start, uint64_t end, int type, int pid,
                                           const void* ofd) const {
    for (const auto& l : o.locks) {
        if (same_owner(l, pid, ofd)) continue;
        if (l.end <= start || end <= l.start) continue;  // no overlap
        if (type == lx::f_wrlck || l.type == lx::f_wrlck) return &l;
    }
    return nullptr;
}

// Replaces the owner's locks over [start, end) with `type` (f_unlck removes),
// splitting and merging like the kernel's posix_lock_file.
void LockTable::apply_locked(Owned& o, uint64_t start, uint64_t end, int type, int pid, const void* ofd) {
    std::vector<FileLock> next;
    next.reserve(o.locks.size() + 2);
    for (const auto& l : o.locks) {
        if (!same_owner(l, pid, ofd) || l.end <= start || end <= l.start) {
            next.push_back(l);
            continue;
        }
        if (l.start < start) next.push_back({l.start, start, l.type, l.pid, l.ofd});  // left remainder
        if (l.end > end) next.push_back({end, l.end, l.type, l.pid, l.ofd});          // right remainder
    }
    if (type != lx::f_unlck) {
        // merge with adjacent same-type locks of this owner
        uint64_t s = start, e = end;
        std::vector<FileLock> merged;
        for (const auto& l : next) {
            if (same_owner(l, pid, ofd) && l.type == type && l.end >= s && l.start <= e) {
                s = std::min(s, l.start);
                e = std::max(e, l.end);
            } else {
                merged.push_back(l);
            }
        }
        merged.push_back({s, e, type, pid, ofd});
        next.swap(merged);
    }
    o.locks.swap(next);
}

int LockTable::set(const LockKey& key, uint64_t start, uint64_t end, int type, int pid, const void* ofd, bool wait) {
    if (type != lx::f_rdlck && type != lx::f_wrlck && type != lx::f_unlck) return -lx::einval;
    if (end <= start) return -lx::einval;
    Waiter local;
    Waiter* w = CurrentWaiter() ? CurrentWaiter() : &local;
    for (;;) {
        wq_.add(w);
        const uint64_t gen = w->prepare();
        {
            std::lock_guard<std::mutex> lk(mu_);
            Owned& o = files_[key];
            if (type == lx::f_unlck || !conflict_locked(o, start, end, type, pid, ofd)) {
                apply_locked(o, start, end, type, pid, ofd);
                if (o.locks.empty() && o.flocks.empty()) files_.erase(key);
                wq_.remove(w);
                wq_.wake();
                return 0;
            }
            if (!wait) {
                wq_.remove(w);
                return -lx::eagain;
            }
        }
        const auto r = w->wait(gen, 0);
        wq_.remove(w);
        if (r == Waiter::Result::Interrupted) return -lx::eintr;
    }
}

int LockTable::get(const LockKey& key, uint64_t start, uint64_t end, int type, int pid, const void* ofd, lx::flock& out) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = files_.find(key);
    const FileLock* c = it == files_.end() ? nullptr : conflict_locked(it->second, start, end, type, pid, ofd);
    if (!c) {
        out.l_type = lx::f_unlck;
        return 0;
    }
    out.l_type = (int16_t)c->type;
    out.l_whence = lx::seek_set;
    out.l_start = (int64_t)c->start;
    out.l_len = c->end == UINT64_MAX ? 0 : (int64_t)(c->end - c->start);
    out.l_pid = c->ofd ? -1 : c->pid;
    return 0;
}

int LockTable::flock(const LockKey& key, const void* ofd, int op) {
    const bool nb = op & lx::lock_nb;
    const int kind = op & ~lx::lock_nb;
    if (kind != lx::lock_sh && kind != lx::lock_ex && kind != lx::lock_un) return -lx::einval;
    Waiter local;
    Waiter* w = CurrentWaiter() ? CurrentWaiter() : &local;
    for (;;) {
        wq_.add(w);
        const uint64_t gen = w->prepare();
        {
            std::lock_guard<std::mutex> lk(mu_);
            Owned& o = files_[key];
            auto mine = std::find_if(o.flocks.begin(), o.flocks.end(), [&](const FileLock& l) { return l.ofd == ofd; });
            if (kind == lx::lock_un) {
                if (mine != o.flocks.end()) o.flocks.erase(mine);
                if (o.locks.empty() && o.flocks.empty()) files_.erase(key);
                wq_.remove(w);
                wq_.wake();
                return 0;
            }
            const int type = kind == lx::lock_ex ? lx::f_wrlck : lx::f_rdlck;
            bool conflict = false;
            for (const auto& l : o.flocks) {
                if (l.ofd == ofd) continue;
                if (type == lx::f_wrlck || l.type == lx::f_wrlck) conflict = true;
            }
            if (!conflict) {
                if (mine != o.flocks.end()) {
                    mine->type = type;
                } else {
                    o.flocks.push_back({0, UINT64_MAX, type, 0, ofd});
                }
                wq_.remove(w);
                wq_.wake();
                return 0;
            }
            if (nb) {
                wq_.remove(w);
                return -lx::eagain;
            }
        }
        const auto r = w->wait(gen, 0);
        wq_.remove(w);
        if (r == Waiter::Result::Interrupted) return -lx::eintr;
    }
}

void LockTable::release_posix(const LockKey& key, int pid) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = files_.find(key);
    if (it == files_.end()) return;
    auto& v = it->second.locks;
    v.erase(std::remove_if(v.begin(), v.end(), [&](const FileLock& l) { return !l.ofd && l.pid == pid; }), v.end());
    if (v.empty() && it->second.flocks.empty()) files_.erase(it);
    wq_.wake();
}

void LockTable::release_pid(int pid) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = files_.begin(); it != files_.end();) {
        auto& v = it->second.locks;
        v.erase(std::remove_if(v.begin(), v.end(), [&](const FileLock& l) { return !l.ofd && l.pid == pid; }), v.end());
        if (v.empty() && it->second.flocks.empty()) {
            it = files_.erase(it);
        } else {
            ++it;
        }
    }
    wq_.wake();
}

void LockTable::release_ofd(const void* ofd) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = files_.begin(); it != files_.end();) {
        auto& v = it->second.locks;
        v.erase(std::remove_if(v.begin(), v.end(), [&](const FileLock& l) { return l.ofd == ofd; }), v.end());
        auto& f = it->second.flocks;
        f.erase(std::remove_if(f.begin(), f.end(), [&](const FileLock& l) { return l.ofd == ofd; }), f.end());
        if (v.empty() && f.empty()) {
            it = files_.erase(it);
        } else {
            ++it;
        }
    }
    wq_.wake();
}

size_t LockTable::count() {
    std::lock_guard<std::mutex> lk(mu_);
    size_t n = 0;
    for (auto& [k, o] : files_) n += o.locks.size() + o.flocks.size();
    return n;
}

}  // namespace rlk
