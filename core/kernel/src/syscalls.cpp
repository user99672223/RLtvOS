// syscalls.cpp — the Linux x86-64 syscall table (C1/C2 set) on top of the
// process/mm/fd objects. Guest pointers are host pointers (one address
// space), so arguments are used directly.
// FEXCore and host headers first (linux_abi.h removes the host macros).
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>

#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>

#include "linux_abi.h"
#include "log.h"
#include "process.h"

namespace rlk {

namespace {

#include "syscall_names.inc"

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
std::array<Handler, kSyscallNamesCount> g_table {};

inline Kernel& K() {
    return Kernel::get();
}

const char* Nr(uint64_t nr) {
    return nr < kSyscallNamesCount && kSyscallNames[nr][0] ? kSyscallNames[nr] : "unknown";
}

// ---- files ---------------------------------------------------------------------

std::shared_ptr<OpenFile> fd_get(Sc& c, int fd) {
    return c.p.fds.get(fd);
}

int64_t do_openat(Sc& c, int dirfd, const char* path, int flags, int mode) {
    if (!path) return -lx::efault;
    const std::string full = K().resolve_path(c.p, dirfd, path);
    const int acc = flags & lx::o_accmode;
    const bool cloexec = flags & lx::o_cloexec;
    // devices
    if (full == "/dev/null" || full == "/dev/zero" || full == "/dev/urandom" || full == "/dev/random") {
        int kind = full == "/dev/null" ? 0 : (full == "/dev/zero" ? 1 : 2);
        return c.p.fds.alloc(std::make_shared<DevNullFile>(kind), cloexec);
    }
    if (full == "/dev/tty") return -lx::enxio;
    lx::stat st {};
    int err = K().stat_path(full, !(flags & lx::o_nofollow), st);
    if (err) {
        if (err == lx::enoent && (flags & lx::o_creat)) return -lx::erofs;
        return -err;
    }
    if ((flags & lx::o_nofollow) && (st.st_mode & lx::s_ifmt) == lx::s_iflnk) return -lx::eloop;
    const bool is_dir = (st.st_mode & lx::s_ifmt) == lx::s_ifdir;
    if ((flags & lx::o_directory) && !is_dir) return -lx::enotdir;
    if (acc != lx::o_rdonly || (flags & lx::o_trunc)) {
        if (is_dir) return -lx::eisdir;
        return -lx::erofs;  // read-only guest filesystem until the overlay lands (Phase C)
    }
    if (is_dir) {
        std::vector<DirEntryInfo> ents;
        err = K().readdir_path(full, ents);
        if (err) return -err;
        return c.p.fds.alloc(std::make_shared<DirFile>(std::move(ents), st, full, flags), cloexec);
    }
    if ((st.st_mode & lx::s_ifmt) != lx::s_ifreg) return -lx::enxio;
    int oerr = 0;
    auto src = K().open_source(full, oerr, nullptr);
    if (!src) return -oerr;
    std::shared_ptr<FileSource> shared(std::move(src));
    return c.p.fds.alloc(std::make_shared<RegularFile>(shared, st, full, flags), cloexec);
}

int64_t sys_openat(Sc& c) {
    c.fmt("%d, \"%s\", 0x%llx, 0%llo", c.fd(0), c.str(c.a[1]), (unsigned long long)c.a[2], (unsigned long long)c.a[3]);
    return do_openat(c, c.fd(0), c.str(c.a[1]), (int)c.a[2], (int)c.a[3]);
}

int64_t sys_open(Sc& c) {
    c.fmt("\"%s\", 0x%llx, 0%llo", c.str(c.a[0]), (unsigned long long)c.a[1], (unsigned long long)c.a[2]);
    return do_openat(c, lx::at_fdcwd, c.str(c.a[0]), (int)c.a[1], (int)c.a[2]);
}

int64_t sys_close(Sc& c) {
    c.fmt("%d", c.fd(0));
    return c.p.fds.close(c.fd(0));
}

int64_t sys_read(Sc& c) {
    c.fmt("%d, 0x%llx, %llu", c.fd(0), (unsigned long long)c.a[1], (unsigned long long)c.a[2]);
    auto f = fd_get(c, c.fd(0));
    if (!f) return -lx::ebadf;
    return f->read(reinterpret_cast<void*>(c.a[1]), (size_t)c.a[2]);
}

int64_t sys_write(Sc& c) {
    auto f = fd_get(c, c.fd(0));
    size_t len = (size_t)c.a[2];
    if (c.fd(0) <= 2 && c.a[1] && len < 200) {
        c.fmt("%d, \"%.*s\", %zu", c.fd(0), (int)len, reinterpret_cast<const char*>(c.a[1]), len);
    } else {
        c.fmt("%d, 0x%llx, %zu", c.fd(0), (unsigned long long)c.a[1], len);
    }
    if (!f) return -lx::ebadf;
    return f->write(reinterpret_cast<const void*>(c.a[1]), len);
}

int64_t sys_pread64(Sc& c) {
    c.fmt("%d, 0x%llx, %llu, %llu", c.fd(0), (unsigned long long)c.a[1], (unsigned long long)c.a[2], (unsigned long long)c.a[3]);
    auto f = fd_get(c, c.fd(0));
    if (!f) return -lx::ebadf;
    return f->pread(reinterpret_cast<void*>(c.a[1]), (size_t)c.a[2], c.a[3]);
}

int64_t sys_pwrite64(Sc& c) {
    c.fmt("%d, 0x%llx, %llu, %llu", c.fd(0), (unsigned long long)c.a[1], (unsigned long long)c.a[2], (unsigned long long)c.a[3]);
    auto f = fd_get(c, c.fd(0));
    if (!f) return -lx::ebadf;
    return f->pwrite(reinterpret_cast<const void*>(c.a[1]), (size_t)c.a[2], c.a[3]);
}

int64_t iov_loop(Sc& c, bool write) {
    auto f = fd_get(c, c.fd(0));
    if (!f) return -lx::ebadf;
    const auto* iov = reinterpret_cast<const lx::iovec*>(c.a[1]);
    int cnt = (int)c.a[2];
    if (cnt < 0 || cnt > 1024) return -lx::einval;
    int64_t total = 0;
    for (int i = 0; i < cnt; i++) {
        if (iov[i].iov_len == 0) continue;
        int64_t n = write ? f->write(reinterpret_cast<const void*>(iov[i].iov_base), (size_t)iov[i].iov_len)
                          : f->read(reinterpret_cast<void*>(iov[i].iov_base), (size_t)iov[i].iov_len);
        if (n < 0) return total ? total : n;
        total += n;
        if ((uint64_t)n < iov[i].iov_len) break;
    }
    return total;
}

int64_t sys_writev(Sc& c) {
    c.fmt("%d, 0x%llx, %d", c.fd(0), (unsigned long long)c.a[1], (int)c.a[2]);
    return iov_loop(c, true);
}

int64_t sys_readv(Sc& c) {
    c.fmt("%d, 0x%llx, %d", c.fd(0), (unsigned long long)c.a[1], (int)c.a[2]);
    return iov_loop(c, false);
}

int64_t sys_lseek(Sc& c) {
    c.fmt("%d, %lld, %d", c.fd(0), (long long)c.a[1], (int)c.a[2]);
    auto f = fd_get(c, c.fd(0));
    if (!f) return -lx::ebadf;
    return f->lseek((int64_t)c.a[1], (int)c.a[2]);
}

int64_t sys_fstat(Sc& c) {
    c.fmt("%d, 0x%llx", c.fd(0), (unsigned long long)c.a[1]);
    auto f = fd_get(c, c.fd(0));
    if (!f) return -lx::ebadf;
    if (!c.a[1]) return -lx::efault;
    return f->fstat(*reinterpret_cast<lx::stat*>(c.a[1]));
}

int64_t do_statat(Sc& c, int dirfd, const char* path, lx::stat* st, int flags) {
    if (!st) return -lx::efault;
    if ((flags & lx::at_empty_path) && (!path || !*path)) {
        auto f = fd_get(c, dirfd);
        if (!f) return -lx::ebadf;
        return f->fstat(*st);
    }
    if (!path) return -lx::efault;
    const std::string full = K().resolve_path(c.p, dirfd, path);
    if (full == "/dev/null" || full == "/dev/zero" || full == "/dev/urandom" || full == "/dev/random") {
        DevNullFile d(full == "/dev/null" ? 0 : (full == "/dev/zero" ? 1 : 2));
        return d.fstat(*st);
    }
    int err = K().stat_path(full, !(flags & lx::at_symlink_nofollow), *st);
    return err ? -err : 0;
}

int64_t sys_newfstatat(Sc& c) {
    c.fmt("%d, \"%s\", 0x%llx, 0x%llx", c.fd(0), c.str(c.a[1]), (unsigned long long)c.a[2], (unsigned long long)c.a[3]);
    return do_statat(c, c.fd(0), c.str(c.a[1]), reinterpret_cast<lx::stat*>(c.a[2]), (int)c.a[3]);
}

int64_t sys_stat(Sc& c) {
    c.fmt("\"%s\", 0x%llx", c.str(c.a[0]), (unsigned long long)c.a[1]);
    return do_statat(c, lx::at_fdcwd, c.str(c.a[0]), reinterpret_cast<lx::stat*>(c.a[1]), 0);
}

int64_t sys_lstat(Sc& c) {
    c.fmt("\"%s\", 0x%llx", c.str(c.a[0]), (unsigned long long)c.a[1]);
    return do_statat(c, lx::at_fdcwd, c.str(c.a[0]), reinterpret_cast<lx::stat*>(c.a[1]), lx::at_symlink_nofollow);
}

int64_t sys_statx(Sc& c) {
    c.fmt("%d, \"%s\", 0x%llx, 0x%llx, 0x%llx", c.fd(0), c.str(c.a[1]), (unsigned long long)c.a[2],
          (unsigned long long)c.a[3], (unsigned long long)c.a[4]);
    return -lx::enosys;  // glibc falls back to newfstatat
}

int64_t do_access(Sc& c, int dirfd, const char* path, int mode) {
    if (!path) return -lx::efault;
    const std::string full = K().resolve_path(c.p, dirfd, path);
    lx::stat st {};
    int err = K().stat_path(full, true, st);
    if (err) return -err;
    if (mode & lx::w_ok) return -lx::erofs;
    if ((mode & lx::x_ok) && !(st.st_mode & 0111) && (st.st_mode & lx::s_ifmt) != lx::s_ifdir) return -lx::eacces;
    return 0;
}

int64_t sys_access(Sc& c) {
    c.fmt("\"%s\", %d", c.str(c.a[0]), (int)c.a[1]);
    return do_access(c, lx::at_fdcwd, c.str(c.a[0]), (int)c.a[1]);
}

int64_t sys_faccessat(Sc& c) {
    c.fmt("%d, \"%s\", %d", c.fd(0), c.str(c.a[1]), (int)c.a[2]);
    return do_access(c, c.fd(0), c.str(c.a[1]), (int)c.a[2]);
}

int64_t do_readlinkat(Sc& c, int dirfd, const char* path, char* buf, size_t size) {
    if (!path || !buf) return -lx::efault;
    const std::string full = K().resolve_path(c.p, dirfd, path);
    std::string target;
    if (full == "/proc/self/exe" || full == "/proc/" + std::to_string(c.p.pid) + "/exe") {
        target = c.p.exe;
    } else if (full == "/proc/self/cwd") {
        target = c.p.cwd;
    } else if (full.rfind("/proc/self/fd/", 0) == 0) {
        int fd = atoi(full.c_str() + 14);
        auto f = fd_get(c, fd);
        if (!f) return -lx::enoent;
        target = f->path;
    } else {
        int err = K().readlink_path(full, target);
        if (err) return -err;
    }
    size_t n = std::min(size, target.size());
    memcpy(buf, target.data(), n);
    return (int64_t)n;
}

int64_t sys_readlink(Sc& c) {
    c.fmt("\"%s\", 0x%llx, %llu", c.str(c.a[0]), (unsigned long long)c.a[1], (unsigned long long)c.a[2]);
    return do_readlinkat(c, lx::at_fdcwd, c.str(c.a[0]), reinterpret_cast<char*>(c.a[1]), (size_t)c.a[2]);
}

int64_t sys_readlinkat(Sc& c) {
    c.fmt("%d, \"%s\", 0x%llx, %llu", c.fd(0), c.str(c.a[1]), (unsigned long long)c.a[2], (unsigned long long)c.a[3]);
    return do_readlinkat(c, c.fd(0), c.str(c.a[1]), reinterpret_cast<char*>(c.a[2]), (size_t)c.a[3]);
}

int64_t sys_getcwd(Sc& c) {
    c.fmt("0x%llx, %llu", (unsigned long long)c.a[0], (unsigned long long)c.a[1]);
    char* buf = reinterpret_cast<char*>(c.a[0]);
    if (!buf) return -lx::efault;
    if (c.p.cwd.size() + 1 > c.a[1]) return -lx::erange;
    memcpy(buf, c.p.cwd.c_str(), c.p.cwd.size() + 1);
    return (int64_t)c.p.cwd.size() + 1;
}

int64_t sys_chdir(Sc& c) {
    c.fmt("\"%s\"", c.str(c.a[0]));
    if (!c.a[0]) return -lx::efault;
    const std::string full = K().resolve_path(c.p, lx::at_fdcwd, c.str(c.a[0]));
    lx::stat st {};
    int err = K().stat_path(full, true, st);
    if (err) return -err;
    if ((st.st_mode & lx::s_ifmt) != lx::s_ifdir) return -lx::enotdir;
    c.p.cwd = full;
    return 0;
}

int64_t sys_fchdir(Sc& c) {
    c.fmt("%d", c.fd(0));
    auto f = fd_get(c, c.fd(0));
    if (!f) return -lx::ebadf;
    if (!f->is_dir()) return -lx::enotdir;
    c.p.cwd = f->path;
    return 0;
}

int64_t sys_getdents64(Sc& c) {
    c.fmt("%d, 0x%llx, %llu", c.fd(0), (unsigned long long)c.a[1], (unsigned long long)c.a[2]);
    auto f = fd_get(c, c.fd(0));
    if (!f) return -lx::ebadf;
    return f->getdents64(reinterpret_cast<void*>(c.a[1]), (size_t)c.a[2]);
}

int64_t sys_fcntl(Sc& c) {
    c.fmt("%d, %d, 0x%llx", c.fd(0), (int)c.a[1], (unsigned long long)c.a[2]);
    int fd = c.fd(0), cmd = (int)c.a[1];
    auto f = fd_get(c, fd);
    if (!f) return -lx::ebadf;
    switch (cmd) {
        case lx::f_dupfd: return c.p.fds.dup(fd, (int)c.a[2], false);
        case lx::f_dupfd_cloexec: return c.p.fds.dup(fd, (int)c.a[2], true);
        case lx::f_getfd: return c.p.fds.get_cloexec(fd);
        case lx::f_setfd: return c.p.fds.set_cloexec(fd, (c.a[2] & lx::fd_cloexec) != 0);
        case lx::f_getfl: return f->oflags;
        case lx::f_setfl: f->oflags = (f->oflags & lx::o_accmode) | ((int)c.a[2] & ~lx::o_accmode); return 0;
        case lx::f_getlk:
        case lx::f_setlk:
        case lx::f_setlkw: return 0;  // no other users of the read-only files yet
        default: return -lx::einval;
    }
}

int64_t sys_dup(Sc& c) {
    c.fmt("%d", c.fd(0));
    return c.p.fds.dup(c.fd(0), 0, false);
}

int64_t sys_dup2(Sc& c) {
    c.fmt("%d, %d", c.fd(0), c.fd(1));
    return c.p.fds.dup2(c.fd(0), c.fd(1), false);
}

int64_t sys_dup3(Sc& c) {
    c.fmt("%d, %d, 0x%llx", c.fd(0), c.fd(1), (unsigned long long)c.a[2]);
    if (c.fd(0) == c.fd(1)) return -lx::einval;
    return c.p.fds.dup2(c.fd(0), c.fd(1), (c.a[2] & lx::o_cloexec) != 0);
}

int64_t sys_ioctl(Sc& c) {
    c.fmt("%d, 0x%llx, 0x%llx", c.fd(0), (unsigned long long)c.a[1], (unsigned long long)c.a[2]);
    auto f = fd_get(c, c.fd(0));
    if (!f) return -lx::ebadf;
    return f->ioctl((unsigned)c.a[1], c.a[2]);
}

// ---- memory ---------------------------------------------------------------------

int64_t sys_mmap(Sc& c) {
    c.fmt("0x%llx, %llu, 0x%llx, 0x%llx, %d, 0x%llx", (unsigned long long)c.a[0], (unsigned long long)c.a[1],
          (unsigned long long)c.a[2], (unsigned long long)c.a[3], c.fd(4), (unsigned long long)c.a[5]);
    const uint64_t addr = c.a[0], len = c.a[1];
    const int prot = (int)c.a[2], flags = (int)c.a[3];
    const int fd = c.fd(4);
    const uint64_t off = c.a[5];
    if (len == 0) return -lx::einval;
    if (off & (lx::page - 1)) return -lx::einval;
    const bool fixed = flags & lx::map_fixed;
    const bool noreplace = flags & lx::map_fixed_noreplace;
    if ((fixed || noreplace) && (addr & (lx::page - 1))) return -lx::einval;
    std::shared_ptr<FileSource> src;
    std::string name;
    uint64_t ino = 0;
    if (!(flags & lx::map_anonymous)) {
        auto f = fd_get(c, fd);
        if (!f) return -lx::ebadf;
        src = f->source();
        if (!src) return -lx::enodev;
        name = f->path;
        ino = src->inode();
    }
    int64_t r = c.p.mm->map(addr, len, prot, fixed || noreplace, noreplace, name, off, ino);
    if (r < 0) return r;
    if (src) {
        // populate [off, off+len) — short files leave the rest zero
        const uint64_t fsz = src->size();
        if (off < fsz) {
            uint64_t n = std::min<uint64_t>(len, fsz - off);
            std::vector<uint8_t> buf(std::min<uint64_t>(n, 1u << 20));
            uint64_t done = 0;
            while (done < n) {
                size_t chunk = (size_t)std::min<uint64_t>(buf.size(), n - done);
                int64_t got = src->pread(buf.data(), chunk, off + done);
                if (got <= 0) break;
                c.p.mm->copy_in((uint64_t)r + done, buf.data(), (size_t)got);
                done += (uint64_t)got;
            }
        }
    }
    return r;
}

int64_t sys_munmap(Sc& c) {
    c.fmt("0x%llx, %llu", (unsigned long long)c.a[0], (unsigned long long)c.a[1]);
    return c.p.mm->unmap(c.a[0], c.a[1]);
}

int64_t sys_mprotect(Sc& c) {
    c.fmt("0x%llx, %llu, 0x%llx", (unsigned long long)c.a[0], (unsigned long long)c.a[1], (unsigned long long)c.a[2]);
    return c.p.mm->protect(c.a[0], c.a[1], (int)c.a[2]);
}

int64_t sys_brk(Sc& c) {
    c.fmt("0x%llx", (unsigned long long)c.a[0]);
    auto& mm = *c.p.mm;
    const uint64_t want = c.a[0];
    if (want == 0 || want < mm.brk_start || want > mm.brk_end) return (int64_t)mm.brk_cur;
    const uint64_t cur_pg = lx::PageUp(mm.brk_cur), want_pg = lx::PageUp(want);
    if (want_pg > cur_pg) {
        // pages above the high-water mark are fresh (zero); reused ones are zeroed
        if (mm.protect(cur_pg, want_pg - cur_pg, lx::prot_read | lx::prot_write) < 0) return (int64_t)mm.brk_cur;
        mm.zero(cur_pg, want_pg - cur_pg);
    } else if (want_pg < cur_pg) {
        mm.protect(want_pg, cur_pg - want_pg, lx::prot_none);
    }
    mm.brk_cur = want;
    return (int64_t)want;
}

int64_t sys_madvise(Sc& c) {
    c.fmt("0x%llx, %llu, %d", (unsigned long long)c.a[0], (unsigned long long)c.a[1], (int)c.a[2]);
    return 0;
}

int64_t sys_mremap(Sc& c) {
    c.fmt("0x%llx, %llu, %llu, 0x%llx", (unsigned long long)c.a[0], (unsigned long long)c.a[1],
          (unsigned long long)c.a[2], (unsigned long long)c.a[3]);
    return -lx::enomem;  // glibc realloc falls back to malloc+copy
}

int64_t sys_msync(Sc& c) {
    c.fmt("0x%llx, %llu, %d", (unsigned long long)c.a[0], (unsigned long long)c.a[1], (int)c.a[2]);
    return 0;
}

// ---- process / thread identity ----------------------------------------------------

int64_t sys_arch_prctl(Sc& c) {
    c.fmt("0x%llx, 0x%llx", (unsigned long long)c.a[0], (unsigned long long)c.a[1]);
    auto& S = c.f->State;
    switch ((int)c.a[0]) {
        case lx::arch_set_fs: S.fs_cached = c.a[1]; return 0;
        case lx::arch_set_gs: S.gs_cached = c.a[1]; return 0;
        case lx::arch_get_fs: *reinterpret_cast<uint64_t*>(c.a[1]) = S.fs_cached; return 0;
        case lx::arch_get_gs: *reinterpret_cast<uint64_t*>(c.a[1]) = S.gs_cached; return 0;
        default: return -lx::einval;
    }
}

int64_t sys_set_tid_address(Sc& c) {
    c.fmt("0x%llx", (unsigned long long)c.a[0]);
    c.t.clear_child_tid = c.a[0];
    return c.t.tid;
}

int64_t sys_set_robust_list(Sc& c) {
    c.fmt("0x%llx, %llu", (unsigned long long)c.a[0], (unsigned long long)c.a[1]);
    c.t.robust_list = c.a[0];
    c.t.robust_list_len = c.a[1];
    return 0;
}

int64_t sys_getpid(Sc& c) { c.args[0] = 0; return c.p.pid; }
int64_t sys_getppid(Sc& c) { c.args[0] = 0; return c.p.ppid; }
int64_t sys_gettid(Sc& c) { c.args[0] = 0; return c.t.tid; }
int64_t sys_getuid(Sc& c) { c.args[0] = 0; return 1000; }
int64_t sys_getpgrp(Sc& c) { c.args[0] = 0; return c.p.pgid; }
int64_t sys_getsid(Sc& c) { c.fmt("%d", (int)c.a[0]); return c.p.sid; }
int64_t sys_setsid(Sc& c) { c.args[0] = 0; c.p.sid = c.p.pgid = c.p.pid; return c.p.pid; }
int64_t sys_setpgid(Sc& c) { c.fmt("%d, %d", (int)c.a[0], (int)c.a[1]); if (c.a[0] == 0 || (int)c.a[0] == c.p.pid) { c.p.pgid = c.a[1] ? (int)c.a[1] : c.p.pid; return 0; } return -lx::esrch; }
int64_t sys_getpgid(Sc& c) { c.fmt("%d", (int)c.a[0]); return c.p.pgid; }
int64_t sys_umask(Sc& c) { c.fmt("0%llo", (unsigned long long)c.a[0]); uint32_t old = c.p.umask; c.p.umask = (uint32_t)c.a[0] & 0777; return old; }

int64_t sys_uname(Sc& c) {
    c.fmt("0x%llx", (unsigned long long)c.a[0]);
    auto* u = reinterpret_cast<lx::utsname*>(c.a[0]);
    if (!u) return -lx::efault;
    memset(u, 0, sizeof *u);
    strncpy(u->sysname, "Linux", 64);
    strncpy(u->nodename, "rltvos", 64);
    strncpy(u->release, "6.1.0-rltvos", 64);
    strncpy(u->version, "#1 SMP PREEMPT_DYNAMIC RLtvOS", 64);
    strncpy(u->machine, "x86_64", 64);
    strncpy(u->domainname, "(none)", 64);
    return 0;
}

int64_t sys_prlimit64(Sc& c) {
    c.fmt("%d, %d, 0x%llx, 0x%llx", (int)c.a[0], (int)c.a[1], (unsigned long long)c.a[2], (unsigned long long)c.a[3]);
    int res = (int)c.a[1];
    if (res < 0 || res >= lx::rlimit_nlimits) return -lx::einval;
    lx::rlimit64 lim {lx::rlim_infinity, lx::rlim_infinity};
    switch (res) {
        case lx::rlimit_stack: lim = {8u << 20, lx::rlim_infinity}; break;
        case lx::rlimit_nofile: lim = {1024, 4096}; break;
        case lx::rlimit_core: lim = {0, lx::rlim_infinity}; break;
        case lx::rlimit_nproc: lim = {4096, 4096}; break;
        case lx::rlimit_memlock: lim = {8u << 20, 8u << 20}; break;
        default: break;
    }
    if (c.a[3]) *reinterpret_cast<lx::rlimit64*>(c.a[3]) = lim;
    return 0;  // new limits accepted and ignored
}

int64_t sys_getrlimit(Sc& c) {
    c.fmt("%d, 0x%llx", (int)c.a[0], (unsigned long long)c.a[1]);
    uint64_t saved[6] = {0, c.a[0], 0, c.a[1], 0, 0};
    Sc c2 {c.t, c.p, c.f, {saved[0], saved[1], saved[2], saved[3], 0, 0}, c.nr, ""};
    int64_t r = sys_prlimit64(c2);
    return r;
}

int64_t sys_setrlimit(Sc& c) {
    c.fmt("%d, 0x%llx", (int)c.a[0], (unsigned long long)c.a[1]);
    return 0;
}

int64_t sys_getrandom(Sc& c) {
    c.fmt("0x%llx, %llu, 0x%llx", (unsigned long long)c.a[0], (unsigned long long)c.a[1], (unsigned long long)c.a[2]);
    if (!c.a[0]) return -lx::efault;
    FillRandom(reinterpret_cast<void*>(c.a[0]), (size_t)c.a[1]);
    return (int64_t)c.a[1];
}

// ---- time ------------------------------------------------------------------------------

int host_clock(int id) {
    switch (id) {
        case lx::clock_realtime:
        case lx::clock_realtime_coarse: return host::kClockRealtime;
        case lx::clock_process_cputime_id: return host::kClockProcessCputime;
        case lx::clock_thread_cputime_id: return host::kClockThreadCputime;
        default: return host::kClockMonotonic;
    }
}

int64_t sys_clock_gettime(Sc& c) {
    c.fmt("%d, 0x%llx", (int)c.a[0], (unsigned long long)c.a[1]);
    auto* ts = reinterpret_cast<lx::timespec*>(c.a[1]);
    if (!ts) return -lx::efault;
    struct timespec h;
    if (clock_gettime((clockid_t)host_clock((int)c.a[0]), &h) != 0) return -lx::einval;
    ts->tv_sec = h.tv_sec;
    ts->tv_nsec = h.tv_nsec;
    return 0;
}

int64_t sys_clock_getres(Sc& c) {
    c.fmt("%d, 0x%llx", (int)c.a[0], (unsigned long long)c.a[1]);
    if (c.a[1]) *reinterpret_cast<lx::timespec*>(c.a[1]) = {0, 1};
    return 0;
}

int64_t sys_gettimeofday(Sc& c) {
    c.fmt("0x%llx, 0x%llx", (unsigned long long)c.a[0], (unsigned long long)c.a[1]);
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    if (c.a[0]) *reinterpret_cast<lx::timeval*>(c.a[0]) = {tv.tv_sec, tv.tv_usec};
    if (c.a[1]) memset(reinterpret_cast<void*>(c.a[1]), 0, 8);
    return 0;
}

int64_t sys_time(Sc& c) {
    c.fmt("0x%llx", (unsigned long long)c.a[0]);
    int64_t now = (int64_t)::time(nullptr);
    if (c.a[0]) *reinterpret_cast<int64_t*>(c.a[0]) = now;
    return now;
}

int64_t sys_nanosleep(Sc& c) {
    c.fmt("0x%llx, 0x%llx", (unsigned long long)c.a[0], (unsigned long long)c.a[1]);
    auto* req = reinterpret_cast<const lx::timespec*>(c.a[0]);
    if (!req) return -lx::efault;
    struct timespec h {(time_t)req->tv_sec, (long)req->tv_nsec};
    ::nanosleep(&h, nullptr);
    if (c.a[1]) *reinterpret_cast<lx::timespec*>(c.a[1]) = {0, 0};
    return 0;
}

int64_t sys_clock_nanosleep(Sc& c) {
    c.fmt("%d, %d, 0x%llx, 0x%llx", (int)c.a[0], (int)c.a[1], (unsigned long long)c.a[2], (unsigned long long)c.a[3]);
    auto* req = reinterpret_cast<const lx::timespec*>(c.a[2]);
    if (!req) return -lx::efault;
    struct timespec h {(time_t)req->tv_sec, (long)req->tv_nsec};
    if (c.a[1] & 1) {  // TIMER_ABSTIME
        struct timespec now;
        clock_gettime((clockid_t)host_clock((int)c.a[0]), &now);
        int64_t ns = (req->tv_sec - now.tv_sec) * 1000000000LL + (req->tv_nsec - now.tv_nsec);
        if (ns <= 0) return 0;
        h = {(time_t)(ns / 1000000000LL), (long)(ns % 1000000000LL)};
    }
    ::nanosleep(&h, nullptr);
    return 0;
}

int64_t sys_sched_yield(Sc& c) {
    c.args[0] = 0;
    std::this_thread::yield();
    return 0;
}

int64_t sys_sched_getaffinity(Sc& c) {
    c.fmt("%d, %llu, 0x%llx", (int)c.a[0], (unsigned long long)c.a[1], (unsigned long long)c.a[2]);
    if (c.a[1] < 8 || !c.a[2]) return -lx::einval;
    memset(reinterpret_cast<void*>(c.a[2]), 0, (size_t)c.a[1]);
    *reinterpret_cast<uint64_t*>(c.a[2]) = 0xF;  // 4 CPUs
    return 8;
}

int64_t sys_sysinfo(Sc& c) {
    c.fmt("0x%llx", (unsigned long long)c.a[0]);
    auto* si = reinterpret_cast<lx::sysinfo*>(c.a[0]);
    if (!si) return -lx::efault;
    memset(si, 0, sizeof *si);
    struct timespec up;
    clock_gettime((clockid_t)host::kClockMonotonic, &up);
    si->uptime = up.tv_sec;
    si->totalram = 4096ull << 20;
    si->freeram = 2048ull << 20;
    si->procs = 8;
    si->mem_unit = 1;
    return 0;
}

// ---- signals (recorded only; delivery is Phase C3) ------------------------------------

int64_t sys_rt_sigaction(Sc& c) {
    c.fmt("%d, 0x%llx, 0x%llx, %llu", (int)c.a[0], (unsigned long long)c.a[1], (unsigned long long)c.a[2],
          (unsigned long long)c.a[3]);
    int sig = (int)c.a[0];
    if (sig < 1 || sig >= lx::nsig || c.a[3] != 8) return -lx::einval;
    if (c.a[2]) *reinterpret_cast<lx::sigaction*>(c.a[2]) = c.p.sigactions[sig];
    if (c.a[1]) {
        if (sig == lx::sigkill || sig == lx::sigstop) return -lx::einval;
        c.p.sigactions[sig] = *reinterpret_cast<const lx::sigaction*>(c.a[1]);
    }
    return 0;
}

int64_t sys_rt_sigprocmask(Sc& c) {
    c.fmt("%d, 0x%llx, 0x%llx, %llu", (int)c.a[0], (unsigned long long)c.a[1], (unsigned long long)c.a[2],
          (unsigned long long)c.a[3]);
    if (c.a[3] != 8) return -lx::einval;
    uint64_t old = c.t.sigmask;
    if (c.a[1]) {
        uint64_t set = *reinterpret_cast<const uint64_t*>(c.a[1]);
        switch ((int)c.a[0]) {
            case lx::sig_block: c.t.sigmask |= set; break;
            case lx::sig_unblock: c.t.sigmask &= ~set; break;
            case lx::sig_setmask: c.t.sigmask = set; break;
            default: return -lx::einval;
        }
        c.t.sigmask &= ~((1ull << (lx::sigkill - 1)) | (1ull << (lx::sigstop - 1)));
    }
    if (c.a[2]) *reinterpret_cast<uint64_t*>(c.a[2]) = old;
    return 0;
}

int64_t sys_sigaltstack(Sc& c) {
    c.fmt("0x%llx, 0x%llx", (unsigned long long)c.a[0], (unsigned long long)c.a[1]);
    if (c.a[1]) memset(reinterpret_cast<void*>(c.a[1]), 0, 24);
    return 0;
}

int64_t sys_kill(Sc& c) {
    c.fmt("%d, %d", (int)c.a[0], (int)c.a[1]);
    int pid = (int)c.a[0], sig = (int)c.a[1];
    if (pid == c.p.pid || pid == 0) {
        if (sig == 0) return 0;
        if (sig == lx::sigkill || sig == lx::sigterm || sig == lx::sigabrt || sig == lx::sigsegv || sig == lx::sigint) {
            K().exit_thread(c.t, c.f, 128 + sig, true);
            return 0;
        }
        return 0;  // other signals to self: no handlers run yet (C3)
    }
    return -lx::esrch;
}

int64_t sys_tgkill(Sc& c) {
    c.fmt("%d, %d, %d", (int)c.a[0], (int)c.a[1], (int)c.a[2]);
    if ((int)c.a[1] == c.t.tid) {
        int sig = (int)c.a[2];
        if (sig == 0) return 0;
        K().exit_thread(c.t, c.f, 128 + sig, true);
        return 0;
    }
    return -lx::esrch;
}

// ---- exit / wait / spawn --------------------------------------------------------------------

int64_t sys_exit(Sc& c) {
    c.fmt("%d", (int)c.a[0]);
    // single-threaded processes: exit == exit_group
    K().exit_thread(c.t, c.f, (int)(c.a[0] & 0xff), c.p.threads.size() <= 1);
    return 0;
}

int64_t sys_exit_group(Sc& c) {
    c.fmt("%d", (int)c.a[0]);
    K().exit_thread(c.t, c.f, (int)(c.a[0] & 0xff), true);
    return 0;
}

int64_t sys_wait4(Sc& c) {
    c.fmt("%d, 0x%llx, 0x%llx, 0x%llx", (int)c.a[0], (unsigned long long)c.a[1], (unsigned long long)c.a[2],
          (unsigned long long)c.a[3]);
    return -lx::echild;
}

int64_t sys_futex(Sc& c) {
    c.fmt("0x%llx, %d, %u, 0x%llx, 0x%llx, %u", (unsigned long long)c.a[0], (int)c.a[1], (unsigned)c.a[2],
          (unsigned long long)c.a[3], (unsigned long long)c.a[4], (unsigned)c.a[5]);
    const int op = (int)c.a[1] & lx::futex_cmd_mask;
    auto* uaddr = reinterpret_cast<uint32_t*>(c.a[0]);
    if (!uaddr) return -lx::efault;
    switch (op) {
        case lx::futex_wake:
        case lx::futex_wake_bitset: return 0;  // nobody waits yet (single-threaded processes)
        case lx::futex_wait:
        case lx::futex_wait_bitset: {
            if (__atomic_load_n(uaddr, __ATOMIC_SEQ_CST) != (uint32_t)c.a[2]) return -lx::eagain;
            // No waker can exist yet: honour a timeout, otherwise give up after 10 ms.
            const auto* to = reinterpret_cast<const lx::timespec*>(c.a[3]);
            struct timespec h {0, 10 * 1000 * 1000};
            if (to && op == lx::futex_wait) h = {(time_t)to->tv_sec, (long)to->tv_nsec};
            ::nanosleep(&h, nullptr);
            return -lx::etimedout;
        }
        default: return -lx::enosys;
    }
}

int64_t sys_socket(Sc& c) {
    c.fmt("%d, %d, %d", (int)c.a[0], (int)c.a[1], (int)c.a[2]);
    return -lx::eafnosupport;
}

int64_t sys_statfs(Sc& c) {
    c.fmt("\"%s\", 0x%llx", c.str(c.a[0]), (unsigned long long)c.a[1]);
    if (!c.a[1]) return -lx::efault;
    lx::stat st {};
    int err = K().stat_path(K().resolve_path(c.p, lx::at_fdcwd, c.str(c.a[0])), true, st);
    if (err) return -err;
    uint64_t* f = reinterpret_cast<uint64_t*>(c.a[1]);
    memset(f, 0, 120);
    f[0] = 0xEF53;          // f_type
    f[1] = 4096;            // f_bsize
    f[2] = 12u << 20;       // f_blocks
    f[3] = f[4] = 4u << 20; // free/avail
    f[5] = 1u << 20;        // files
    f[6] = 1u << 19;        // ffree
    f[8] = 255;             // namelen
    f[9] = 4096;            // frsize
    return 0;
}

int64_t sys_fstatfs(Sc& c) {
    c.fmt("%d, 0x%llx", c.fd(0), (unsigned long long)c.a[1]);
    if (!fd_get(c, c.fd(0))) return -lx::ebadf;
    uint64_t* f = reinterpret_cast<uint64_t*>(c.a[1]);
    if (!f) return -lx::efault;
    memset(f, 0, 120);
    f[0] = 0xEF53;
    f[1] = 4096;
    f[8] = 255;
    f[9] = 4096;
    return 0;
}

int64_t sys_prctl(Sc& c) {
    c.fmt("%d, 0x%llx, 0x%llx", (int)c.a[0], (unsigned long long)c.a[1], (unsigned long long)c.a[2]);
    switch ((int)c.a[0]) {
        case 15: {  // PR_SET_NAME
            const char* n = c.str(c.a[1]);
            c.p.comm = std::string(n).substr(0, 15);
            return 0;
        }
        case 16: {  // PR_GET_NAME
            char* out = reinterpret_cast<char*>(c.a[1]);
            if (!out) return -lx::efault;
            strncpy(out, c.p.comm.c_str(), 16);
            return 0;
        }
        case 3: case 4:   // PR_GET/SET_DUMPABLE
        case 38: case 39: // PR_SET/GET_NO_NEW_PRIVS
        case 1: case 2:   // PDEATHSIG
            return 0;
        default: return -lx::einval;
    }
}

int64_t sys_ret0(Sc& c) { c.fmt("0x%llx, 0x%llx", (unsigned long long)c.a[0], (unsigned long long)c.a[1]); return 0; }
int64_t sys_enosys(Sc& c) { c.fmt("0x%llx, 0x%llx, 0x%llx", (unsigned long long)c.a[0], (unsigned long long)c.a[1], (unsigned long long)c.a[2]); return -lx::enosys; }
int64_t sys_erofs(Sc& c) { c.fmt("0x%llx, 0x%llx", (unsigned long long)c.a[0], (unsigned long long)c.a[1]); return -lx::erofs; }
int64_t sys_enotsup(Sc& c) { c.fmt("\"%s\", 0x%llx", c.str(c.a[0]), (unsigned long long)c.a[1]); return -lx::eopnotsupp; }
int64_t sys_getcpu(Sc& c) {
    c.fmt("0x%llx, 0x%llx", (unsigned long long)c.a[0], (unsigned long long)c.a[1]);
    if (c.a[0]) *reinterpret_cast<uint32_t*>(c.a[0]) = 0;
    if (c.a[1]) *reinterpret_cast<uint32_t*>(c.a[1]) = 0;
    return 0;
}

void set(uint64_t nr, Handler h) {
    if (nr < g_table.size()) g_table[nr] = h;
}

void init_table() {
    static bool done = false;
    if (done) return;
    done = true;
    set(0, sys_read); set(1, sys_write); set(2, sys_open); set(3, sys_close); set(4, sys_stat); set(5, sys_fstat);
    set(6, sys_lstat); set(8, sys_lseek); set(9, sys_mmap); set(10, sys_mprotect); set(11, sys_munmap);
    set(12, sys_brk); set(13, sys_rt_sigaction); set(14, sys_rt_sigprocmask); set(15, sys_enosys /*rt_sigreturn*/);
    set(16, sys_ioctl); set(17, sys_pread64); set(18, sys_pwrite64); set(19, sys_readv); set(20, sys_writev);
    set(21, sys_access); set(24, sys_sched_yield); set(25, sys_mremap); set(26, sys_msync); set(28, sys_madvise);
    set(32, sys_dup); set(33, sys_dup2); set(35, sys_nanosleep); set(39, sys_getpid); set(41, sys_socket);
    set(60, sys_exit); set(61, sys_wait4); set(62, sys_kill); set(63, sys_uname); set(72, sys_fcntl);
    set(73, sys_ret0 /*flock*/); set(74, sys_ret0 /*fsync*/); set(75, sys_ret0 /*fdatasync*/); set(77, sys_erofs /*ftruncate*/);
    set(79, sys_getcwd); set(80, sys_chdir); set(81, sys_fchdir); set(89, sys_readlink); set(95, sys_umask);
    set(96, sys_gettimeofday); set(97, sys_getrlimit); set(98, sys_ret0 /*getrusage*/); set(99, sys_sysinfo);
    set(100, sys_ret0 /*times*/); set(102, sys_getuid); set(104, sys_getuid /*getgid*/); set(107, sys_getuid /*geteuid*/);
    set(108, sys_getuid /*getegid*/); set(109, sys_setpgid); set(110, sys_getppid); set(111, sys_getpgrp);
    set(112, sys_setsid); set(121, sys_getpgid); set(124, sys_getsid); set(131, sys_sigaltstack);
    set(137, sys_statfs); set(138, sys_fstatfs); set(157, sys_prctl); set(158, sys_arch_prctl);
    set(160, sys_setrlimit); set(186, sys_gettid); set(201, sys_time); set(202, sys_futex);
    set(203, sys_ret0 /*sched_setaffinity*/); set(204, sys_sched_getaffinity); set(217, sys_getdents64);
    set(218, sys_set_tid_address); set(221, sys_ret0 /*fadvise64*/); set(228, sys_clock_gettime);
    set(229, sys_clock_getres); set(230, sys_clock_nanosleep); set(231, sys_exit_group); set(234, sys_tgkill);
    set(257, sys_openat); set(262, sys_newfstatat); set(267, sys_readlinkat); set(269, sys_faccessat);
    set(273, sys_set_robust_list); set(274, sys_enosys /*get_robust_list*/); set(292, sys_dup3);
    set(302, sys_prlimit64); set(309, sys_getcpu); set(318, sys_getrandom); set(332, sys_statx);
    set(334, sys_enosys /*rseq*/); set(439, sys_faccessat /*faccessat2*/);
    // xattr family: not supported on this filesystem
    set(191, sys_enotsup); set(192, sys_enotsup); set(193, sys_enotsup); set(194, sys_enotsup); set(195, sys_enotsup);
    set(196, sys_enotsup);
}

}  // namespace

void Kernel::handle_syscall(GuestThread& t, void* frame_) {
    init_table();
    auto* f = static_cast<FEXCore::Core::CpuStateFrame*>(frame_);
    auto& S = f->State;
    using namespace FEXCore::X86State;
    Sc c {t, *t.proc, f, {S.gregs[REG_RDI], S.gregs[REG_RSI], S.gregs[REG_RDX], S.gregs[REG_R10], S.gregs[REG_R8], S.gregs[REG_R9]},
          S.gregs[REG_RAX], ""};
    t_current_proc = t.proc;
    t_current_thread = &t;
    t.syscalls++;
    t.proc->syscalls++;
    total_syscalls++;
    if (t.proc->state.load() == (int)ProcState::Dead) {  // killed from the outside
        exit_thread(t, f, 137, true);
        return;
    }
    int64_t ret;
    if (c.nr < g_table.size() && g_table[c.nr]) {
        ret = g_table[c.nr](c);
    } else {
        c.fmt("0x%llx, 0x%llx, 0x%llx, 0x%llx", (unsigned long long)c.a[0], (unsigned long long)c.a[1],
              (unsigned long long)c.a[2], (unsigned long long)c.a[3]);
        ret = -lx::enosys;
    }
    if (t.exited) {
        strace(t, Nr(c.nr), c.args, 0);
        S.gregs[REG_RAX] = 0;  // rip already points at the hlt page
        return;
    }
    strace(t, Nr(c.nr), c.args, ret);
    S.gregs[REG_RAX] = (uint64_t)ret;
    S.rip += 2;  // past `syscall`
}

}  // namespace rlk
