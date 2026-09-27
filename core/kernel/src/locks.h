// locks.h — advisory file locks between guest processes: POSIX record locks
// (fcntl F_SETLK/F_SETLKW/F_GETLK, owned by a process, dropped when that
// process closes any descriptor of the file), open-file-description locks
// (F_OFD_*, owned by the open file) and flock(2) (owned by the open file,
// whole file). All guest processes live in one host process, so these are
// bookkeeping tables keyed by the file's identity (st_dev, st_ino); nothing
// reaches the host. wineserver's lock file and Wine's F_GETLK probe of it
// (C5) need the real conflict semantics.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

#include "linux_abi.h"
#include "waitq.h"

namespace rlk {

struct LockKey {
    uint64_t dev, ino;
    bool operator<(const LockKey& o) const { return dev != o.dev ? dev < o.dev : ino < o.ino; }
};

struct FileLock {
    uint64_t start;         // [start, end), end = UINT64_MAX for "to EOF"
    uint64_t end;
    int type;               // lx::f_rdlck / lx::f_wrlck
    int pid;                // POSIX owner (0 for OFD/flock)
    const void* ofd;        // OFD/flock owner: the open file description (nullptr for POSIX)
};

class LockTable {
public:
    static LockTable& get();

    // fcntl(F_SETLK / F_SETLKW / F_OFD_*): sets, converts or removes (f_unlck)
    // a lock. wait: block until it can be taken (interruptible: -EINTR).
    // Returns 0, -EAGAIN when a conflicting lock is held, -EINTR.
    int set(const LockKey& key, uint64_t start, uint64_t end, int type, int pid, const void* ofd, bool wait);
    // fcntl(F_GETLK): if a conflicting lock exists, describes it in `out`
    // (l_type != f_unlck); else out.l_type = f_unlck. Always 0.
    int get(const LockKey& key, uint64_t start, uint64_t end, int type, int pid, const void* ofd, lx::flock& out);
    // flock(2): LOCK_SH/LOCK_EX/LOCK_UN (+LOCK_NB → -EAGAIN instead of waiting).
    int flock(const LockKey& key, const void* ofd, int op);

    // POSIX locks of `pid` on the file go away when it closes any fd of it.
    void release_posix(const LockKey& key, int pid);
    void release_pid(int pid);           // process exit
    void release_ofd(const void* ofd);   // the open file description is gone

    size_t count();

private:
    LockTable() = default;
    struct Owned {
        std::vector<FileLock> locks;
        std::vector<FileLock> flocks;   // flock(2) namespace, whole-file
    };
    // mu_ held: the first lock conflicting with the request, or nullptr.
    const FileLock* conflict_locked(const Owned& o, uint64_t start, uint64_t end, int type, int pid, const void* ofd) const;
    void apply_locked(Owned& o, uint64_t start, uint64_t end, int type, int pid, const void* ofd);
    static bool same_owner(const FileLock& l, int pid, const void* ofd) {
        return l.ofd ? l.ofd == ofd : (ofd == nullptr && l.pid == pid);
    }

    std::mutex mu_;
    std::map<LockKey, Owned> files_;
    WaitQueue wq_;   // every unlock wakes every waiter; they re-check
};

}  // namespace rlk
