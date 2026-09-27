// syscalls.h — shared by the syscall handler files (syscalls.cpp,
// syscalls_fs.cpp, syscalls_io.cpp). Guest pointers are host pointers (one
// address space), so arguments are used directly. Handlers return the
// Linux result (>= 0 or -errno) and fill Sc::args for the strace log.
// FEXCore and host headers first (linux_abi.h removes the host macros).
#pragma once

#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <memory>

#include "linux_abi.h"
#include "locks.h"
#include "process.h"

namespace rlk {

struct Sc {
    GuestThread& t;
    GuestProcess& p;
    FEXCore::Core::CpuStateFrame* f;
    uint64_t a[6];
    uint64_t nr;
    char args[512];

    void fmt(const char* format, ...) __attribute__((format(printf, 2, 3))) {
        va_list ap;
        va_start(ap, format);
        vsnprintf(args, sizeof args, format, ap);
        va_end(ap);
    }
    const char* str(uint64_t p) const { return p ? reinterpret_cast<const char*>(p) : "(null)"; }
    int fd(int i) const { return (int)(int32_t)a[i]; }
};

using Handler = int64_t (*)(Sc&);
using SetFn = void (*)(uint64_t nr, Handler h);

inline Kernel& K() {
    return Kernel::get();
}

inline std::shared_ptr<OpenFile> fd_get(Sc& c, int fd) {
    return c.p.fds.get(fd);
}

// (st_dev, st_ino) of an open file: the record-lock key.
inline bool lock_key_of(OpenFile& f, LockKey& key) {
    lx::stat st {};
    if (f.fstat(st) != 0) return false;
    key = {st.st_dev, st.st_ino};
    return true;
}

// Monotonic deadline (ns) from a relative timespec; 0 when ts is null.
inline int64_t deadline_from(const lx::timespec* ts) {
    if (!ts) return 0;
    int64_t d = MonotonicNs() + ts->tv_sec * 1000000000LL + ts->tv_nsec;
    return d <= 0 ? 1 : d;
}

// The nonblocking flag of an fd (O_NONBLOCK) or of one call (MSG_DONTWAIT).
inline bool nonblocking(const OpenFile& f, int msg_flags = 0) {
    return (f.oflags & lx::o_nonblock) || (msg_flags & lx::msg_dontwait);
}

void register_fs_syscalls(SetFn set);   // syscalls_fs.cpp
void register_io_syscalls(SetFn set);   // syscalls_io.cpp

// The open(2) path, shared with creat/openat2-less callers.
int64_t do_openat(Sc& c, int dirfd, const char* path, int flags, int mode);

}  // namespace rlk
