// syscalls.cpp — the Linux x86-64 syscall table on top of the process/mm/fd
// objects: files, memory, identity, time, signals, processes, futex. The
// filesystem namespace lives in syscalls_fs.cpp, polling/sockets/timers in
// syscalls_io.cpp; Sc and the shared helpers are in syscalls.h.
#include "syscalls.h"

#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <thread>

#include "futex.h"
#include "log.h"
#include "overlay.h"
#include "pipe.h"

namespace rlk {

namespace {

#include "syscall_names.inc"

std::array<Handler, kSyscallNamesCount> g_table {};

const char* Nr(uint64_t nr) {
    return nr < kSyscallNamesCount && kSyscallNames[nr][0] ? kSyscallNames[nr] : "unknown";
}

// ---- files ---------------------------------------------------------------------

int64_t sys_close(Sc& c) {
    c.fmt("%d", c.fd(0));
    auto f = fd_get(c, c.fd(0));
    int r = c.p.fds.close(c.fd(0));
    if (r == 0 && f && f->has_locks) {  // POSIX: any close by the owner drops its locks on the file
        LockKey key;
        if (lock_key_of(*f, key)) LockTable::get().release_posix(key, c.p.pid);
    }
    return r;
}

int64_t sys_read(Sc& c) {
    c.fmt("%d, 0x%llx, %llu", c.fd(0), (unsigned long long)c.a[1], (unsigned long long)c.a[2]);
    auto f = fd_get(c, c.fd(0));
    if (!f) return -lx::ebadf;
    int64_t r = f->read(reinterpret_cast<void*>(c.a[1]), (size_t)c.a[2]);
    if (r == -lx::eintr) c.t.restartable = true;
    return r;
}

int64_t sys_write(Sc& c) {
    auto f = fd_get(c, c.fd(0));
    size_t len = (size_t)c.a[2];
    if (c.a[1]) {
        c.fmt("%d, \"%s\", %zu", c.fd(0), StraceStr(reinterpret_cast<const void*>(c.a[1]), len).c_str(), len);
    } else {
        c.fmt("%d, NULL, %zu", c.fd(0), len);
    }
    if (!f) return -lx::ebadf;
    int64_t r = f->write(reinterpret_cast<const void*>(c.a[1]), len);
    if (r == -lx::epipe) K().send_signal_thread(c.t, lx::sigpipe, nullptr);
    if (r == -lx::eintr) c.t.restartable = true;
    return r;
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
    const unsigned req = (unsigned)c.a[1];
    if (req == lx::fioclex) return c.p.fds.set_cloexec(c.fd(0), true);
    if (req == lx::fionclex) return c.p.fds.set_cloexec(c.fd(0), false);
    return f->ioctl(req, c.a[2]);
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
    const char* strace_note = "";
    struct NoteAppender {
        Sc& c;
        const char*& note;
        ~NoteAppender() {
            if (*note) strncat(c.args, note, sizeof c.args - strlen(c.args) - 1);
        }
    } note_appender {c, strace_note};
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
    if (src && (flags & lx::map_shared)) {
        if (auto tmp = std::dynamic_pointer_cast<TmpFileSource>(src)) {
            // MAP_SHARED of an upper file: alias the file's pages when the
            // mapping is host-page aligned and exclusively ours (Wine's 64 KB
            // views are), else a private copy written back on munmap/msync/
            // exit (DECISIONS 2026-09-27).
            auto node = tmp->node();
            const uint64_t hp = AddressSpace::host_page();
            const uint64_t hlen = AddressSpace::host_up(lx::PageUp(len));
            bool aliased = false;
            if (((uint64_t)r % hp) == 0 && (off % hp) == 0) {
                std::lock_guard<std::mutex> lk(node->data->mu);
                if (node->data->ensure_store((size_t)(off + hlen))) {
                    aliased = c.p.mm->alias_shared((uint64_t)r, hlen, node->data->store.get(), off, prot);
                }
            }
            if (aliased) {
                strace_note = " (shared alias)";
                return r;
            }
            if (prot & lx::prot_write) K().add_shared_map(c.p, (uint64_t)r, lx::PageUp(len), node, off);
        }
    }
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
    K().writeback_shared(c.p, c.a[0], c.a[1], true);
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
    K().writeback_shared(c.p, c.a[0], c.a[1], false);
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

// Interruptible sleep until the monotonic deadline; writes the remainder.
int64_t do_sleep(Sc& c, int64_t deadline_ns, lx::timespec* rem) {
    for (;;) {
        const uint64_t gen = c.t.waiter.prepare();
        const int64_t now = MonotonicNs();
        if (now >= deadline_ns) return 0;
        auto r = c.t.waiter.wait(gen, deadline_ns);
        if (r == Waiter::Result::Timeout) return 0;
        if (r == Waiter::Result::Interrupted) {
            const int64_t left = deadline_ns - MonotonicNs();
            if (rem && left > 0) *rem = {left / 1000000000LL, left % 1000000000LL};
            return -lx::eintr;  // nanosleep is never restarted (the remainder tells the caller)
        }
    }
}

int64_t sys_nanosleep(Sc& c) {
    c.fmt("0x%llx, 0x%llx", (unsigned long long)c.a[0], (unsigned long long)c.a[1]);
    auto* req = reinterpret_cast<const lx::timespec*>(c.a[0]);
    if (!req) return -lx::efault;
    if (req->tv_nsec < 0 || req->tv_nsec >= 1000000000LL || req->tv_sec < 0) return -lx::einval;
    auto* rem = reinterpret_cast<lx::timespec*>(c.a[1]);
    if (rem) *rem = {0, 0};
    return do_sleep(c, MonotonicNs() + req->tv_sec * 1000000000LL + req->tv_nsec, rem);
}

int64_t sys_clock_nanosleep(Sc& c) {
    c.fmt("%d, %d, 0x%llx, 0x%llx", (int)c.a[0], (int)c.a[1], (unsigned long long)c.a[2], (unsigned long long)c.a[3]);
    auto* req = reinterpret_cast<const lx::timespec*>(c.a[2]);
    if (!req) return -lx::efault;
    if (req->tv_nsec < 0 || req->tv_nsec >= 1000000000LL || req->tv_sec < 0) return -lx::einval;
    int64_t deadline;
    if (c.a[1] & 1) {  // TIMER_ABSTIME in the given clock: convert to monotonic
        struct timespec now;
        clock_gettime((clockid_t)host_clock((int)c.a[0]), &now);
        const int64_t ns = (req->tv_sec - now.tv_sec) * 1000000000LL + (req->tv_nsec - now.tv_nsec);
        if (ns <= 0) return 0;
        deadline = MonotonicNs() + ns;
    } else {
        deadline = MonotonicNs() + req->tv_sec * 1000000000LL + req->tv_nsec;
    }
    auto* rem = (c.a[1] & 1) ? nullptr : reinterpret_cast<lx::timespec*>(c.a[3]);
    return do_sleep(c, deadline, rem);
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
    const uint64_t rsp = c.f->State.gregs[FEXCore::X86State::REG_RSP];
    const bool on_it = !(c.t.altstack_flags & lx::ss_disable) && c.t.altstack_size && rsp > c.t.altstack_sp &&
                       rsp <= c.t.altstack_sp + c.t.altstack_size;
    if (c.a[1]) {
        auto* old = reinterpret_cast<lx::stack_t*>(c.a[1]);
        old->ss_sp = c.t.altstack_sp;
        old->ss_size = c.t.altstack_size;
        old->ss_flags = (c.t.altstack_flags & lx::ss_disable) ? lx::ss_disable : (on_it ? lx::ss_onstack : 0);
        old->pad = 0;
    }
    if (c.a[0]) {
        if (on_it) return -lx::eperm;
        const auto* ss = reinterpret_cast<const lx::stack_t*>(c.a[0]);
        if (ss->ss_flags & lx::ss_disable) {
            c.t.altstack_flags = lx::ss_disable;
            c.t.altstack_sp = c.t.altstack_size = 0;
        } else {
            if (ss->ss_flags & ~lx::ss_onstack) return -lx::einval;
            if (ss->ss_size < lx::minsigstksz) return -lx::enomem;
            c.t.altstack_sp = ss->ss_sp;
            c.t.altstack_size = ss->ss_size;
            c.t.altstack_flags = 0;
        }
    }
    return 0;
}

// Signal to a process id (>0), the caller's group (0), everyone (-1) or a group (<-1).
int64_t send_to_pid(Sc& c, int pid, int sig, const lx::siginfo* info) {
    Kernel& k = K();
    std::vector<GuestProcess*> targets;
    {
        std::lock_guard<std::mutex> lk(k.mu);
        for (auto& [id, p] : k.procs) {
            if (p->state.load() != (int)ProcState::Running) continue;
            if (pid > 0 && id != pid) continue;
            if (pid == 0 && p->pgid != c.p.pgid) continue;
            if (pid == -1 && id == c.p.pid) continue;
            if (pid < -1 && p->pgid != -pid) continue;
            targets.push_back(p.get());
        }
    }
    if (targets.empty()) return -lx::esrch;
    for (auto* p : targets) {
        if (sig == lx::sigkill && p != &c.p) {
            // Uncatchable: the target's threads exit at their next syscall /
            // wakeup; ones spinning in JIT code are kicked out of it.
            p->term_signal = lx::sigkill;
            p->state = (int)ProcState::Dead;
            std::lock_guard<std::mutex> lk(k.mu);
            for (auto& th : p->threads) {
                th->waiter.notify();
                k.kick_thread_locked(*th);
            }
            continue;
        }
        k.send_signal(*p, sig, info);
    }
    return 0;
}

int64_t sys_kill(Sc& c) {
    c.fmt("%d, %d", (int)c.a[0], (int)c.a[1]);
    const int pid = (int)c.a[0], sig = (int)c.a[1];
    if (sig < 0 || sig >= lx::nsig) return -lx::einval;
    lx::siginfo si {};
    si.si_signo = sig;
    si.si_code = lx::si_user;
    si.u.kill.pid = c.p.pid;
    si.u.kill.uid = 1000;
    if (sig == 0) return send_to_pid(c, pid, 0, nullptr) == -lx::esrch ? -lx::esrch : 0;
    return send_to_pid(c, pid, sig, &si);
}

int64_t do_tkill(Sc& c, int tgid, int tid, int sig) {
    if (sig < 0 || sig >= lx::nsig || tid <= 0) return -lx::einval;
    Kernel& k = K();
    GuestThread* target = nullptr;
    {
        std::lock_guard<std::mutex> lk(k.mu);
        for (auto& [id, p] : k.procs) {
            if (tgid > 0 && id != tgid) continue;
            for (auto& th : p->threads) {
                if (th->tid == tid && !th->exited) target = th.get();
            }
        }
    }
    if (!target) return -lx::esrch;
    if (sig == 0) return 0;
    lx::siginfo si {};
    si.si_signo = sig;
    si.si_code = lx::si_tkill;
    si.u.kill.pid = c.p.pid;
    si.u.kill.uid = 1000;
    return k.send_signal_thread(*target, sig, &si);
}

int64_t sys_tgkill(Sc& c) {
    c.fmt("%d, %d, %d", (int)c.a[0], (int)c.a[1], (int)c.a[2]);
    return do_tkill(c, (int)c.a[0], (int)c.a[1], (int)c.a[2]);
}

int64_t sys_tkill(Sc& c) {
    c.fmt("%d, %d", (int)c.a[0], (int)c.a[1]);
    return do_tkill(c, 0, (int)c.a[0], (int)c.a[1]);
}

int64_t sys_rt_sigreturn(Sc& c) {
    c.args[0] = 0;
    return K().sigreturn(c.t, c.f);
}

int64_t sys_rt_sigsuspend(Sc& c) {
    c.fmt("0x%llx, %llu", (unsigned long long)c.a[0], (unsigned long long)c.a[1]);
    if (!c.a[0] || c.a[1] != 8) return -lx::einval;
    // The temporary mask is in force until the handler ran; the handler's
    // frame saves the *old* mask, so sigreturn restores it.
    const uint64_t old = c.t.sigmask;
    c.t.sigmask = *reinterpret_cast<const uint64_t*>(c.a[0]) & ~(lx::sigbit(lx::sigkill) | lx::sigbit(lx::sigstop));
    int64_t r = K().wait_for_signal(c.t);
    // deliver_signals() runs after we return: it must see the temporary
    // mask (to pick the signal) and save `old` in the frame.
    c.t.saved_mask_valid = true;
    c.t.saved_mask = old;
    return r;
}

int64_t sys_pause(Sc& c) {
    c.args[0] = 0;
    return K().wait_for_signal(c.t);
}

// ---- exit / wait / clone / exec --------------------------------------------------------------

int64_t sys_exit(Sc& c) {
    c.fmt("%d", (int)c.a[0]);
    K().exit_thread(c.t, c.f, (int)(c.a[0] & 0xff), false);
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
    if (c.a[3]) memset(reinterpret_cast<void*>(c.a[3]), 0, 144);  // rusage
    return K().do_wait4(c.t, (int)c.a[0], reinterpret_cast<int32_t*>(c.a[1]), (int)c.a[2]);
}

int64_t sys_clone(Sc& c) {
    // x86-64 order: flags, stack, parent_tid, child_tid, tls
    c.fmt("0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx", (unsigned long long)c.a[0], (unsigned long long)c.a[1],
          (unsigned long long)c.a[2], (unsigned long long)c.a[3], (unsigned long long)c.a[4]);
    return K().do_clone(c.t, c.f, c.a[0], c.a[1], c.a[2], c.a[3], c.a[4]);
}

int64_t sys_fork(Sc& c) {
    c.args[0] = 0;
    return K().do_clone(c.t, c.f, lx::sigchld, 0, 0, 0, 0);
}

int64_t sys_vfork(Sc& c) {
    c.args[0] = 0;
    return K().do_clone(c.t, c.f, lx::clone_vm | lx::clone_vfork | lx::sigchld, 0, 0, 0, 0);
}

std::vector<std::string> read_string_array(uint64_t p) {
    std::vector<std::string> out;
    if (!p) return out;
    auto* arr = reinterpret_cast<const char* const*>(p);
    for (size_t i = 0; arr[i] && i < 65536; i++) out.emplace_back(arr[i]);
    return out;
}

int64_t sys_execve(Sc& c) {
    c.fmt("\"%s\", 0x%llx, 0x%llx", c.str(c.a[0]), (unsigned long long)c.a[1], (unsigned long long)c.a[2]);
    if (!c.a[0]) return -lx::efault;
    const std::string path = K().resolve_path(c.p, lx::at_fdcwd, c.str(c.a[0]));
    auto argv = read_string_array(c.a[1]);
    auto envp = read_string_array(c.a[2]);
    return K().do_execve(c.t, c.f, path, std::move(argv), std::move(envp));
}

int64_t do_pipe(Sc& c, int32_t* fds, int flags) {
    if (!fds) return -lx::efault;
    if (flags & ~(lx::o_nonblock | lx::o_cloexec)) return -lx::einval;
    std::shared_ptr<PipeFile> rd, wr;
    MakePipe(rd, wr);
    if (flags & lx::o_nonblock) {
        rd->oflags |= lx::o_nonblock;
        wr->oflags |= lx::o_nonblock;
    }
    int r = c.p.fds.alloc(rd, flags & lx::o_cloexec);
    if (r < 0) return r;
    int w = c.p.fds.alloc(wr, flags & lx::o_cloexec);
    if (w < 0) {
        c.p.fds.close(r);
        return w;
    }
    fds[0] = r;
    fds[1] = w;
    return 0;
}

int64_t sys_pipe(Sc& c) {
    c.fmt("0x%llx", (unsigned long long)c.a[0]);
    return do_pipe(c, reinterpret_cast<int32_t*>(c.a[0]), 0);
}

int64_t sys_pipe2(Sc& c) {
    c.fmt("0x%llx, 0x%llx", (unsigned long long)c.a[0], (unsigned long long)c.a[1]);
    return do_pipe(c, reinterpret_cast<int32_t*>(c.a[0]), (int)c.a[1]);
}

int64_t sys_futex(Sc& c) {
    c.fmt("0x%llx, %d, %u, 0x%llx, 0x%llx, %u", (unsigned long long)c.a[0], (int)c.a[1], (unsigned)c.a[2],
          (unsigned long long)c.a[3], (unsigned long long)c.a[4], (unsigned)c.a[5]);
    const int op = (int)c.a[1] & lx::futex_cmd_mask;
    const bool realtime = (int)c.a[1] & lx::futex_clock_realtime;
    auto* uaddr = reinterpret_cast<uint32_t*>(c.a[0]);
    if (!uaddr || (c.a[0] & 3)) return -lx::einval;
    auto& ft = FutexTable::get();
    switch (op) {
        case lx::futex_wait:
        case lx::futex_wait_bitset: {
            const auto* to = reinterpret_cast<const lx::timespec*>(c.a[3]);
            int64_t deadline = 0;
            if (to) {
                if (to->tv_nsec < 0 || to->tv_nsec >= 1000000000LL) return -lx::einval;
                const int64_t ns = to->tv_sec * 1000000000LL + to->tv_nsec;
                if (op == lx::futex_wait) {
                    deadline = MonotonicNs() + ns;  // relative
                } else if (realtime) {              // absolute CLOCK_REALTIME → monotonic
                    struct timespec now;
                    clock_gettime((clockid_t)host::kClockRealtime, &now);
                    deadline = MonotonicNs() + (ns - (now.tv_sec * 1000000000LL + now.tv_nsec));
                } else {                            // absolute CLOCK_MONOTONIC (host clock == our MonotonicNs)
                    struct timespec now;
                    clock_gettime((clockid_t)host::kClockMonotonic, &now);
                    deadline = MonotonicNs() + (ns - (now.tv_sec * 1000000000LL + now.tv_nsec));
                }
                if (deadline <= 0) deadline = 1;
            }
            const uint32_t bits = op == lx::futex_wait ? ~0u : (uint32_t)c.a[5];
            int r = ft.wait(uaddr, (uint32_t)c.a[2], deadline, bits);
            if (r == -lx::eintr) c.t.restartable = true;
            return r;
        }
        case lx::futex_wake: return ft.wake(uaddr, (int)c.a[2], ~0u);
        case lx::futex_wake_bitset: return ft.wake(uaddr, (int)c.a[2], (uint32_t)c.a[5]);
        case lx::futex_requeue: return ft.requeue(uaddr, (int)c.a[2], (int)c.a[3], reinterpret_cast<uint32_t*>(c.a[4]), false, 0);
        case lx::futex_cmp_requeue:
            return ft.requeue(uaddr, (int)c.a[2], (int)c.a[3], reinterpret_cast<uint32_t*>(c.a[4]), true, (uint32_t)c.a[5]);
        case lx::futex_wake_op: return ft.wake_op(uaddr, (int)c.a[2], reinterpret_cast<uint32_t*>(c.a[4]), (int)c.a[3], (uint32_t)c.a[5]);
        default: return -lx::enosys;
    }
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
int64_t sys_enotsup(Sc& c) { c.fmt("\"%s\", 0x%llx", c.str(c.a[0]), (unsigned long long)c.a[1]); return -lx::eopnotsupp; }
int64_t sys_getcpu(Sc& c) {
    c.fmt("0x%llx, 0x%llx", (unsigned long long)c.a[0], (unsigned long long)c.a[1]);
    if (c.a[0]) *reinterpret_cast<uint32_t*>(c.a[0]) = 0;
    if (c.a[1]) *reinterpret_cast<uint32_t*>(c.a[1]) = 0;
    return 0;
}

int64_t sys_setid(Sc& c) {
    c.fmt("%d, %d, %d", (int)c.a[0], (int)c.a[1], (int)c.a[2]);
    for (int i = 0; i < 3; i++) {
        if (c.a[i] != (uint64_t)-1 && (uint32_t)c.a[i] != (uint32_t)-1 && (uint32_t)c.a[i] != 1000) return -lx::eperm;
        if (c.nr == 105 || c.nr == 106) break;  // setuid/setgid: one argument
        if ((c.nr == 113 || c.nr == 114) && i == 1) break;  // setreuid/setregid: two
    }
    return 0;
}
int64_t sys_getresid(Sc& c) {
    c.fmt("0x%llx, 0x%llx, 0x%llx", (unsigned long long)c.a[0], (unsigned long long)c.a[1], (unsigned long long)c.a[2]);
    for (int i = 0; i < 3; i++) {
        if (c.a[i]) *reinterpret_cast<uint32_t*>(c.a[i]) = 1000;
    }
    return 0;
}
int64_t sys_getgroups(Sc& c) {
    c.fmt("%d, 0x%llx", (int)c.a[0], (unsigned long long)c.a[1]);
    if ((int)c.a[0] == 0) return 1;
    if ((int)c.a[0] < 1) return -lx::einval;
    if (c.a[1]) *reinterpret_cast<uint32_t*>(c.a[1]) = 1000;
    return 1;
}
int64_t sys_sched_getparam(Sc& c) {
    c.fmt("%d, 0x%llx", (int)c.a[0], (unsigned long long)c.a[1]);
    if (c.a[1]) *reinterpret_cast<int32_t*>(c.a[1]) = 0;
    return 0;
}
int64_t sys_sched_rr_get_interval(Sc& c) {
    c.fmt("%d, 0x%llx", (int)c.a[0], (unsigned long long)c.a[1]);
    if (c.a[1]) *reinterpret_cast<lx::timespec*>(c.a[1]) = {0, 4000000};
    return 0;
}
int64_t sys_mincore(Sc& c) {
    c.fmt("0x%llx, %llu, 0x%llx", (unsigned long long)c.a[0], (unsigned long long)c.a[1], (unsigned long long)c.a[2]);
    if (!c.a[2]) return -lx::efault;
    if (c.a[0] & (lx::page - 1)) return -lx::einval;
    if (!c.p.mm->is_mapped(c.a[0], c.a[1])) return -lx::enomem;
    memset(reinterpret_cast<void*>(c.a[2]), 1, (size_t)((c.a[1] + lx::page - 1) / lx::page));
    return 0;
}

void set(uint64_t nr, Handler h) {
    if (nr < g_table.size()) g_table[nr] = h;
}

void init_table() {
    static bool done = false;
    if (done) return;
    done = true;
    set(0, sys_read); set(1, sys_write); set(3, sys_close); set(4, sys_stat); set(5, sys_fstat);
    set(6, sys_lstat); set(8, sys_lseek); set(9, sys_mmap); set(10, sys_mprotect); set(11, sys_munmap);
    set(12, sys_brk); set(13, sys_rt_sigaction); set(14, sys_rt_sigprocmask); set(15, sys_rt_sigreturn);
    set(22, sys_pipe); set(34, sys_pause); set(56, sys_clone); set(57, sys_fork); set(58, sys_vfork); set(59, sys_execve);
    set(130, sys_rt_sigsuspend); set(200, sys_tkill); set(293, sys_pipe2); set(435, sys_enosys /*clone3 → glibc uses clone*/);
    set(247, sys_enosys /*waitid*/);
    set(16, sys_ioctl); set(17, sys_pread64); set(18, sys_pwrite64); set(19, sys_readv); set(20, sys_writev);
    set(24, sys_sched_yield); set(25, sys_mremap); set(26, sys_msync); set(28, sys_madvise);
    set(32, sys_dup); set(33, sys_dup2); set(35, sys_nanosleep); set(39, sys_getpid);
    set(60, sys_exit); set(61, sys_wait4); set(62, sys_kill); set(63, sys_uname);
    set(74, sys_ret0 /*fsync*/); set(75, sys_ret0 /*fdatasync*/);
    set(79, sys_getcwd); set(80, sys_chdir); set(81, sys_fchdir); set(89, sys_readlink); set(95, sys_umask);
    set(96, sys_gettimeofday); set(97, sys_getrlimit); set(98, sys_ret0 /*getrusage*/); set(99, sys_sysinfo);
    set(100, sys_ret0 /*times*/); set(102, sys_getuid); set(104, sys_getuid /*getgid*/); set(107, sys_getuid /*geteuid*/);
    set(108, sys_getuid /*getegid*/); set(109, sys_setpgid); set(110, sys_getppid); set(111, sys_getpgrp);
    set(112, sys_setsid); set(121, sys_getpgid); set(124, sys_getsid); set(131, sys_sigaltstack);
    set(157, sys_prctl); set(158, sys_arch_prctl);
    set(160, sys_setrlimit); set(186, sys_gettid); set(201, sys_time); set(202, sys_futex);
    set(203, sys_ret0 /*sched_setaffinity*/); set(204, sys_sched_getaffinity); set(217, sys_getdents64);
    set(218, sys_set_tid_address); set(221, sys_ret0 /*fadvise64*/); set(228, sys_clock_gettime);
    set(229, sys_clock_getres); set(230, sys_clock_nanosleep); set(231, sys_exit_group); set(234, sys_tgkill);
    set(262, sys_newfstatat); set(267, sys_readlinkat);
    set(273, sys_set_robust_list); set(274, sys_enosys /*get_robust_list*/); set(292, sys_dup3);
    set(302, sys_prlimit64); set(309, sys_getcpu); set(318, sys_getrandom); set(332, sys_statx);
    set(334, sys_enosys /*rseq*/);
    // identity setters: uid 1000 is the only user
    set(105, sys_setid); set(106, sys_setid); set(113, sys_setid); set(114, sys_setid); set(117, sys_setid);
    set(119, sys_setid); set(116, sys_ret0 /*setgroups*/); set(115, sys_getgroups); set(118, sys_getresid);
    set(120, sys_getresid);
    // scheduling: everything is SCHED_OTHER at priority 0
    set(140, sys_ret0 /*getpriority*/); set(141, sys_ret0 /*setpriority*/); set(142, sys_ret0 /*sched_setparam*/);
    set(143, sys_sched_getparam); set(144, sys_ret0 /*sched_setscheduler*/); set(145, sys_ret0 /*sched_getscheduler*/);
    set(146, sys_ret0 /*sched_get_priority_max*/); set(147, sys_ret0 /*sched_get_priority_min*/);
    set(148, sys_sched_rr_get_interval); set(314, sys_enosys /*sched_setattr*/); set(315, sys_enosys /*sched_getattr*/);
    set(27, sys_mincore);
    // xattr family: not supported on this filesystem
    set(191, sys_enotsup); set(192, sys_enotsup); set(193, sys_enotsup); set(194, sys_enotsup); set(195, sys_enotsup);
    set(196, sys_enotsup);
    register_fs_syscalls(set);
    register_io_syscalls(set);
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
    t.syscall_nr = c.nr;
    t.restartable = false;
    t.no_handler_restart = false;
    if (t.proc->state.load() != (int)ProcState::Running) {
        // The group is exiting (exit_group elsewhere, SIGKILL, killall): leave quietly.
        const int code = t.proc->term_signal ? 128 + t.proc->term_signal : t.proc->exit_code;
        exit_thread(t, f, code, t.proc->state.load() == (int)ProcState::Dead, t.proc->term_signal);
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
    if (t.no_retval) {  // execve, rt_sigreturn: the state is the new context
        t.no_retval = false;
        strace(t, Nr(c.nr), c.args, ret);
        if (has_deliverable(t)) deliver_signals(t, f, 0);
        return;
    }
    strace(t, Nr(c.nr), c.args, ret);
    S.gregs[REG_RAX] = (uint64_t)ret;
    S.rip += 2;  // past `syscall`
    if (has_deliverable(t)) deliver_signals(t, f, ret);
    if (t.saved_mask_valid) {  // rt_sigsuspend without a handler run: mask back
        t.saved_mask_valid = false;
        t.sigmask = t.saved_mask;
    }
}

}  // namespace rlk
