// syscalls_fs.cpp — the file-system half of the syscall table on top of the
// overlay (overlay.h): open with creation/truncation/append, the namespace
// calls (mkdir, unlink, rename, link, symlink, mknod, chmod, chown, utimens,
// truncate), record locks (fcntl F_SETLK/F_GETLK/F_OFD_*, flock), statfs,
// memfd_create, sendfile.
#include "syscalls.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "overlay.h"
#include "pipe.h"

namespace rlk {

namespace {

// An O_PATH descriptor: identity only (fstat, openat(dirfd), fchdir).
class PathFile final : public OpenFile {
public:
    PathFile(const lx::stat& st, std::string guest_path) : st_(st) {
        path = std::move(guest_path);
        oflags = lx::o_path;
    }
    int fstat(lx::stat& st) override {
        st = st_;
        return 0;
    }
    bool is_dir() const override { return (st_.st_mode & lx::s_ifmt) == lx::s_ifdir; }

private:
    lx::stat st_;
};

bool is_type(const lx::stat& st, uint32_t type) {
    return (st.st_mode & lx::s_ifmt) == type;
}

// /proc/<pid>/mem: the target's memory (same host task: memcpy through its
// address-space bookkeeping, regardless of guest protection — FOLL_FORCE).
// Wine's Read/WriteProcessMemory between guest processes come here.
class ProcMemFile final : public OpenFile {
public:
    ProcMemFile(GuestProcess* p, std::string guest_path, int oflags) : p_(p) {
        path = std::move(guest_path);
        this->oflags = oflags;
    }
    int64_t read(void* buf, size_t len) override {
        std::lock_guard<std::mutex> lk(mu);
        int64_t r = pread(buf, len, pos_);
        if (r > 0) pos_ += (uint64_t)r;
        return r;
    }
    int64_t write(const void* buf, size_t len) override {
        std::lock_guard<std::mutex> lk(mu);
        int64_t r = pwrite(buf, len, pos_);
        if (r > 0) pos_ += (uint64_t)r;
        return r;
    }
    int64_t pread(void* buf, size_t len, uint64_t off) override { return access(buf, len, off, false); }
    int64_t pwrite(const void* buf, size_t len, uint64_t off) override { return access(const_cast<void*>(buf), len, off, true); }
    int64_t lseek(int64_t off, int whence) override {
        std::lock_guard<std::mutex> lk(mu);
        if (whence == lx::seek_set) pos_ = (uint64_t)off;
        else if (whence == lx::seek_cur) pos_ += (uint64_t)off;
        else return -lx::einval;
        return (int64_t)pos_;
    }
    int fstat(lx::stat& st) override {
        FillStat(st, lx::s_ifreg | 0600, 0, 0, 0x90000 + (uint64_t)p_->pid, 0, 1000, 1000);
        return 0;
    }

private:
    int64_t access(void* buf, size_t len, uint64_t off, bool write) {
        if ((write && (oflags & lx::o_accmode) == lx::o_rdonly) || (!write && (oflags & lx::o_accmode) == lx::o_wronly)) {
            return -lx::ebadf;
        }
        std::shared_ptr<AddressSpace> mm;
        {
            std::lock_guard<std::mutex> lk(K().mu);
            mm = p_->mm;
        }
        if (!mm) return -lx::eio;  // exited and reaped
        size_t done = 0;
        while (done < len) {
            const uint64_t a = off + done;
            const Vma v = mm->find(a);
            if (v.end == 0) break;
            const size_t chunk = (size_t)std::min<uint64_t>(len - done, v.end - a);
            const bool ok = write ? mm->copy_in(a, static_cast<const uint8_t*>(buf) + done, chunk)
                                  : mm->copy_out(a, static_cast<uint8_t*>(buf) + done, chunk);
            if (!ok) break;
            done += chunk;
        }
        if (done == 0 && len) return -lx::eio;
        return (int64_t)done;
    }

    GuestProcess* p_;
    uint64_t pos_ = 0;
};

// /proc/self/mem, /proc/<pid>/mem (true when the path was one of them).
int64_t open_proc_mem(Sc& c, const std::string& full, int flags, bool& handled) {
    handled = false;
    if (full.rfind("/proc/", 0) != 0 || full.size() < 10 || full.compare(full.size() - 4, 4, "/mem") != 0) return 0;
    const std::string comp = full.substr(6, full.size() - 10);
    GuestProcess* target = nullptr;
    if (comp == "self" || comp == "thread-self") {
        target = &c.p;
    } else if (!comp.empty() && comp.size() <= 7 && comp.find_first_not_of("0123456789") == std::string::npos) {
        std::lock_guard<std::mutex> lk(K().mu);
        target = K().find(atoi(comp.c_str()));
    } else {
        return 0;
    }
    handled = true;
    if (!target) return -lx::enoent;
    return c.p.fds.alloc(std::make_shared<ProcMemFile>(target, full, flags & lx::o_accmode), flags & lx::o_cloexec);
}

// Character devices the kernel provides itself (the rootfs image may lack
// the nodes; the overlay may carry them as lower entries).
int64_t open_device(Sc& c, const std::string& full, int flags, bool& handled) {
    handled = true;
    const bool cloexec = flags & lx::o_cloexec;
    if (full == "/dev/null") return c.p.fds.alloc(std::make_shared<DevNullFile>(0), cloexec);
    if (full == "/dev/zero" || full == "/dev/full") return c.p.fds.alloc(std::make_shared<DevNullFile>(1), cloexec);
    if (full == "/dev/urandom" || full == "/dev/random") return c.p.fds.alloc(std::make_shared<DevNullFile>(2), cloexec);
    if (full == "/dev/tty" || full == "/dev/console") return -lx::enxio;
    if (full == "/dev/ptmx") return -lx::enodev;
    handled = false;
    return 0;
}

int64_t open_dir(Sc& c, const std::string& full, const lx::stat& st, int flags) {
    std::vector<DirEntryInfo> ents;
    int err = K().readdir_path(full, ents);
    if (err) return -err;
    return c.p.fds.alloc(std::make_shared<DirFile>(std::move(ents), st, full, flags), flags & lx::o_cloexec);
}

}  // namespace

// ---- open ----------------------------------------------------------------------------

int64_t do_openat(Sc& c, int dirfd, const char* path, int flags, int mode) {
    if (!path) return -lx::efault;
    const std::string full = K().resolve_path(c.p, dirfd, path);
    const int acc = flags & lx::o_accmode;
    const bool cloexec = flags & lx::o_cloexec;
    const bool want_write = acc != lx::o_rdonly || (flags & lx::o_trunc);
    const bool follow = !(flags & lx::o_nofollow);
    if ((flags & lx::o_tmpfile) == lx::o_tmpfile) return -lx::eopnotsupp;

    if (full.rfind("/dev/", 0) == 0) {
        bool handled;
        int64_t r = open_device(c, full, flags, handled);
        if (handled) return r;
    }
    if (full.rfind("/proc/", 0) == 0) {
        bool handled;
        int64_t r = open_proc_mem(c, full, flags, handled);
        if (handled) return r;
    }

    Overlay& ov = K().overlay();
    Lookup l;
    int err;
    if (flags & lx::o_creat) {
        err = ov.create(full, mode & ~c.p.umask & 07777, (flags & lx::o_excl) != 0, l);
        if (err) return -err;
    } else {
        err = ov.lookup(full, follow, l);
        if (err) return -err;
    }
    const lx::stat& st = l.st;
    if (!follow && is_type(st, lx::s_iflnk)) {
        if (flags & lx::o_path) return c.p.fds.alloc(std::make_shared<PathFile>(st, l.canon), cloexec);
        return -lx::eloop;
    }
    if (flags & lx::o_path) return c.p.fds.alloc(std::make_shared<PathFile>(st, l.canon), cloexec);
    if (is_type(st, lx::s_ifdir)) {
        if (want_write) return -lx::eisdir;
        return open_dir(c, l.canon, st, flags);
    }
    if (flags & lx::o_directory) return -lx::enotdir;
    if (is_type(st, lx::s_ifsock)) return -lx::enxio;
    if (is_type(st, lx::s_ififo)) {
        if (!l.upper || !l.upper->fifo) return -lx::enxio;
        auto f = std::make_shared<PipeFile>(l.upper->fifo, acc != lx::o_rdonly);
        if (flags & lx::o_nonblock) f->oflags |= lx::o_nonblock;
        f->path = l.canon;
        return c.p.fds.alloc(f, cloexec);
    }
    if (is_type(st, lx::s_ifchr) || is_type(st, lx::s_ifblk)) return -lx::enxio;
    if (!is_type(st, lx::s_ifreg)) return -lx::enxio;

    if (l.upper || want_write) {
        std::shared_ptr<TmpNode> node = l.upper;
        if (!node) {  // a lower file opened for writing: copy-up
            err = ov.for_write(l.canon, follow, node);
            if (err) return -err;
        }
        if (flags & lx::o_trunc) {
            std::lock_guard<std::mutex> lk(node->data->mu);
            node->data->resize(0);
            node->mtime = node->ctime = Overlay::now_sec();
        }
        return c.p.fds.alloc(std::make_shared<TmpFile>(node, l.canon, flags & ~(lx::o_creat | lx::o_excl | lx::o_trunc)), cloexec);
    }
    int oerr = 0;
    auto src = K().open_source(l.canon, oerr, nullptr);
    if (!src) return -oerr;
    std::shared_ptr<FileSource> shared(std::move(src));
    return c.p.fds.alloc(std::make_shared<RegularFile>(shared, st, l.canon, flags), cloexec);
}

namespace {

int64_t sys_openat(Sc& c) {
    c.fmt("%d, \"%s\", 0x%llx, 0%llo", c.fd(0), c.str(c.a[1]), (unsigned long long)c.a[2], (unsigned long long)c.a[3]);
    return do_openat(c, c.fd(0), c.str(c.a[1]), (int)c.a[2], (int)c.a[3]);
}

int64_t sys_open(Sc& c) {
    c.fmt("\"%s\", 0x%llx, 0%llo", c.str(c.a[0]), (unsigned long long)c.a[1], (unsigned long long)c.a[2]);
    return do_openat(c, lx::at_fdcwd, c.str(c.a[0]), (int)c.a[1], (int)c.a[2]);
}

int64_t sys_creat(Sc& c) {
    c.fmt("\"%s\", 0%llo", c.str(c.a[0]), (unsigned long long)c.a[1]);
    return do_openat(c, lx::at_fdcwd, c.str(c.a[0]), lx::o_creat | lx::o_wronly | lx::o_trunc, (int)c.a[1]);
}

// ---- access ---------------------------------------------------------------------------

int64_t do_access(Sc& c, int dirfd, const char* path, int mode, int flags) {
    if (!path) return -lx::efault;
    const std::string full = K().resolve_path(c.p, dirfd, path);
    lx::stat st {};
    int err = K().stat_path(full, !(flags & lx::at_symlink_nofollow), st);
    if (err) return -err;
    if ((mode & lx::w_ok) && full.rfind("/proc/", 0) == 0) return -lx::eacces;
    if ((mode & lx::x_ok) && !(st.st_mode & 0111) && (st.st_mode & lx::s_ifmt) != lx::s_ifdir) return -lx::eacces;
    return 0;
}

int64_t sys_access(Sc& c) {
    c.fmt("\"%s\", %d", c.str(c.a[0]), (int)c.a[1]);
    return do_access(c, lx::at_fdcwd, c.str(c.a[0]), (int)c.a[1], 0);
}

int64_t sys_faccessat(Sc& c) {
    c.fmt("%d, \"%s\", %d", c.fd(0), c.str(c.a[1]), (int)c.a[2]);
    return do_access(c, c.fd(0), c.str(c.a[1]), (int)c.a[2], 0);
}

int64_t sys_faccessat2(Sc& c) {
    c.fmt("%d, \"%s\", %d, 0x%llx", c.fd(0), c.str(c.a[1]), (int)c.a[2], (unsigned long long)c.a[3]);
    return do_access(c, c.fd(0), c.str(c.a[1]), (int)c.a[2], (int)c.a[3]);
}

// ---- namespace ------------------------------------------------------------------------

int64_t do_mkdirat(Sc& c, int dirfd, const char* path, int mode) {
    if (!path) return -lx::efault;
    int err = K().overlay().mkdir(K().resolve_path(c.p, dirfd, path), (uint32_t)mode & ~c.p.umask);
    return -err;
}

int64_t sys_mkdir(Sc& c) {
    c.fmt("\"%s\", 0%llo", c.str(c.a[0]), (unsigned long long)c.a[1]);
    return do_mkdirat(c, lx::at_fdcwd, c.str(c.a[0]), (int)c.a[1]);
}

int64_t sys_mkdirat(Sc& c) {
    c.fmt("%d, \"%s\", 0%llo", c.fd(0), c.str(c.a[1]), (unsigned long long)c.a[2]);
    return do_mkdirat(c, c.fd(0), c.str(c.a[1]), (int)c.a[2]);
}

int64_t sys_rmdir(Sc& c) {
    c.fmt("\"%s\"", c.str(c.a[0]));
    return -K().overlay().rmdir(K().resolve_path(c.p, lx::at_fdcwd, c.str(c.a[0])));
}

int64_t do_unlinkat(Sc& c, int dirfd, const char* path, int flags) {
    if (!path) return -lx::efault;
    const std::string full = K().resolve_path(c.p, dirfd, path);
    if (flags & lx::at_removedir) return -K().overlay().rmdir(full);
    return -K().overlay().unlink(full);
}

int64_t sys_unlink(Sc& c) {
    c.fmt("\"%s\"", c.str(c.a[0]));
    return do_unlinkat(c, lx::at_fdcwd, c.str(c.a[0]), 0);
}

int64_t sys_unlinkat(Sc& c) {
    c.fmt("%d, \"%s\", 0x%llx", c.fd(0), c.str(c.a[1]), (unsigned long long)c.a[2]);
    return do_unlinkat(c, c.fd(0), c.str(c.a[1]), (int)c.a[2]);
}

int64_t do_renameat(Sc& c, int odir, const char* from, int ndir, const char* to, unsigned flags) {
    if (!from || !to) return -lx::efault;
    return -K().overlay().rename(K().resolve_path(c.p, odir, from), K().resolve_path(c.p, ndir, to), flags);
}

int64_t sys_rename(Sc& c) {
    c.fmt("\"%s\", \"%s\"", c.str(c.a[0]), c.str(c.a[1]));
    return do_renameat(c, lx::at_fdcwd, c.str(c.a[0]), lx::at_fdcwd, c.str(c.a[1]), 0);
}

int64_t sys_renameat(Sc& c) {
    c.fmt("%d, \"%s\", %d, \"%s\"", c.fd(0), c.str(c.a[1]), c.fd(2), c.str(c.a[3]));
    return do_renameat(c, c.fd(0), c.str(c.a[1]), c.fd(2), c.str(c.a[3]), 0);
}

int64_t sys_renameat2(Sc& c) {
    c.fmt("%d, \"%s\", %d, \"%s\", 0x%llx", c.fd(0), c.str(c.a[1]), c.fd(2), c.str(c.a[3]), (unsigned long long)c.a[4]);
    return do_renameat(c, c.fd(0), c.str(c.a[1]), c.fd(2), c.str(c.a[3]), (unsigned)c.a[4]);
}

int64_t do_linkat(Sc& c, int odir, const char* from, int ndir, const char* to, int flags) {
    if (!from || !to) return -lx::efault;
    std::string src;
    if ((flags & lx::at_empty_path) && !*from) {
        auto f = fd_get(c, odir);
        if (!f) return -lx::ebadf;
        src = f->path;
    } else {
        src = K().resolve_path(c.p, odir, from);
    }
    return -K().overlay().link(src, K().resolve_path(c.p, ndir, to));
}

int64_t sys_link(Sc& c) {
    c.fmt("\"%s\", \"%s\"", c.str(c.a[0]), c.str(c.a[1]));
    return do_linkat(c, lx::at_fdcwd, c.str(c.a[0]), lx::at_fdcwd, c.str(c.a[1]), 0);
}

int64_t sys_linkat(Sc& c) {
    c.fmt("%d, \"%s\", %d, \"%s\", 0x%llx", c.fd(0), c.str(c.a[1]), c.fd(2), c.str(c.a[3]), (unsigned long long)c.a[4]);
    return do_linkat(c, c.fd(0), c.str(c.a[1]), c.fd(2), c.str(c.a[3]), (int)c.a[4]);
}

int64_t sys_symlink(Sc& c) {
    c.fmt("\"%s\", \"%s\"", c.str(c.a[0]), c.str(c.a[1]));
    if (!c.a[0] || !c.a[1]) return -lx::efault;
    return -K().overlay().symlink(c.str(c.a[0]), K().resolve_path(c.p, lx::at_fdcwd, c.str(c.a[1])));
}

int64_t sys_symlinkat(Sc& c) {
    c.fmt("\"%s\", %d, \"%s\"", c.str(c.a[0]), c.fd(1), c.str(c.a[2]));
    if (!c.a[0] || !c.a[2]) return -lx::efault;
    return -K().overlay().symlink(c.str(c.a[0]), K().resolve_path(c.p, c.fd(1), c.str(c.a[2])));
}

int64_t do_mknodat(Sc& c, int dirfd, const char* path, uint32_t mode) {
    if (!path) return -lx::efault;
    const uint32_t type = mode & lx::s_ifmt;
    if (type == 0 || type == lx::s_ifreg) {
        int64_t fd = do_openat(c, dirfd, path, lx::o_creat | lx::o_excl | lx::o_wronly, (int)(mode & 07777));
        if (fd < 0) return fd;
        c.p.fds.close((int)fd);
        return 0;
    }
    std::shared_ptr<TmpNode> node;
    return -K().overlay().mknod(K().resolve_path(c.p, dirfd, path), (mode & lx::s_ifmt) | (mode & ~c.p.umask & 07777), node);
}

int64_t sys_mknod(Sc& c) {
    c.fmt("\"%s\", 0%llo, 0x%llx", c.str(c.a[0]), (unsigned long long)c.a[1], (unsigned long long)c.a[2]);
    return do_mknodat(c, lx::at_fdcwd, c.str(c.a[0]), (uint32_t)c.a[1]);
}

int64_t sys_mknodat(Sc& c) {
    c.fmt("%d, \"%s\", 0%llo, 0x%llx", c.fd(0), c.str(c.a[1]), (unsigned long long)c.a[2], (unsigned long long)c.a[3]);
    return do_mknodat(c, c.fd(0), c.str(c.a[1]), (uint32_t)c.a[2]);
}

int64_t do_fchmodat(Sc& c, int dirfd, const char* path, uint32_t mode, int flags) {
    if (!path) return -lx::efault;
    return -K().overlay().chmod(K().resolve_path(c.p, dirfd, path), !(flags & lx::at_symlink_nofollow), mode);
}

int64_t sys_chmod(Sc& c) {
    c.fmt("\"%s\", 0%llo", c.str(c.a[0]), (unsigned long long)c.a[1]);
    return do_fchmodat(c, lx::at_fdcwd, c.str(c.a[0]), (uint32_t)c.a[1], 0);
}

int64_t sys_fchmodat(Sc& c) {
    c.fmt("%d, \"%s\", 0%llo", c.fd(0), c.str(c.a[1]), (unsigned long long)c.a[2]);
    return do_fchmodat(c, c.fd(0), c.str(c.a[1]), (uint32_t)c.a[2], 0);
}

int64_t sys_fchmodat2(Sc& c) {
    c.fmt("%d, \"%s\", 0%llo, 0x%llx", c.fd(0), c.str(c.a[1]), (unsigned long long)c.a[2], (unsigned long long)c.a[3]);
    return do_fchmodat(c, c.fd(0), c.str(c.a[1]), (uint32_t)c.a[2], (int)c.a[3]);
}

int64_t sys_fchmod(Sc& c) {
    c.fmt("%d, 0%llo", c.fd(0), (unsigned long long)c.a[1]);
    auto f = fd_get(c, c.fd(0));
    if (!f) return -lx::ebadf;
    if (f->path.empty() || f->path[0] != '/') return 0;  // pipes, sockets: nothing to change
    return -K().overlay().chmod(f->path, true, (uint32_t)c.a[1]);
}

int64_t do_fchownat(Sc& c, int dirfd, const char* path, uint32_t uid, uint32_t gid, int flags) {
    if (!path) return -lx::efault;
    return -K().overlay().chown(K().resolve_path(c.p, dirfd, path), !(flags & lx::at_symlink_nofollow), uid, gid);
}

int64_t sys_chown(Sc& c) {
    c.fmt("\"%s\", %d, %d", c.str(c.a[0]), (int)c.a[1], (int)c.a[2]);
    return do_fchownat(c, lx::at_fdcwd, c.str(c.a[0]), (uint32_t)c.a[1], (uint32_t)c.a[2], 0);
}

int64_t sys_lchown(Sc& c) {
    c.fmt("\"%s\", %d, %d", c.str(c.a[0]), (int)c.a[1], (int)c.a[2]);
    return do_fchownat(c, lx::at_fdcwd, c.str(c.a[0]), (uint32_t)c.a[1], (uint32_t)c.a[2], lx::at_symlink_nofollow);
}

int64_t sys_fchownat(Sc& c) {
    c.fmt("%d, \"%s\", %d, %d, 0x%llx", c.fd(0), c.str(c.a[1]), (int)c.a[2], (int)c.a[3], (unsigned long long)c.a[4]);
    return do_fchownat(c, c.fd(0), c.str(c.a[1]), (uint32_t)c.a[2], (uint32_t)c.a[3], (int)c.a[4]);
}

int64_t sys_fchown(Sc& c) {
    c.fmt("%d, %d, %d", c.fd(0), (int)c.a[1], (int)c.a[2]);
    auto f = fd_get(c, c.fd(0));
    if (!f) return -lx::ebadf;
    if (f->path.empty() || f->path[0] != '/') return 0;
    return -K().overlay().chown(f->path, true, (uint32_t)c.a[1], (uint32_t)c.a[2]);
}

int64_t sys_truncate(Sc& c) {
    c.fmt("\"%s\", %llu", c.str(c.a[0]), (unsigned long long)c.a[1]);
    if (!c.a[0]) return -lx::efault;
    if ((int64_t)c.a[1] < 0) return -lx::einval;
    return -K().overlay().truncate(K().resolve_path(c.p, lx::at_fdcwd, c.str(c.a[0])), c.a[1]);
}

int64_t sys_ftruncate(Sc& c) {
    c.fmt("%d, %llu", c.fd(0), (unsigned long long)c.a[1]);
    auto f = fd_get(c, c.fd(0));
    if (!f) return -lx::ebadf;
    if ((int64_t)c.a[1] < 0) return -lx::einval;
    return f->ftruncate(c.a[1]);
}

int64_t sys_fallocate(Sc& c) {
    c.fmt("%d, %d, %llu, %llu", c.fd(0), (int)c.a[1], (unsigned long long)c.a[2], (unsigned long long)c.a[3]);
    auto f = fd_get(c, c.fd(0));
    if (!f) return -lx::ebadf;
    if ((int)c.a[1] != 0) return -lx::eopnotsupp;  // only the plain "make it this big" mode
    lx::stat st {};
    if (f->fstat(st) != 0) return -lx::ebadf;
    const uint64_t end = c.a[2] + c.a[3];
    if ((uint64_t)st.st_size >= end) return 0;
    return f->ftruncate(end);
}

int64_t do_utimensat(Sc& c, int dirfd, const char* path, const lx::timespec* times, int flags) {
    int64_t atime = lx::utime_now, mtime = lx::utime_now;
    if (times) {
        auto conv = [](const lx::timespec& t) -> int64_t {
            if (t.tv_nsec == lx::utime_now) return lx::utime_now;
            if (t.tv_nsec == lx::utime_omit) return lx::utime_omit;
            return t.tv_sec;
        };
        atime = conv(times[0]);
        mtime = conv(times[1]);
    }
    std::string full;
    if (!path || !*path) {
        auto f = fd_get(c, dirfd);
        if (!f) return -lx::ebadf;
        if (f->path.empty() || f->path[0] != '/') return 0;
        full = f->path;
    } else {
        full = K().resolve_path(c.p, dirfd, path);
    }
    return -K().overlay().utimens(full, !(flags & lx::at_symlink_nofollow), atime, mtime);
}

int64_t sys_utimensat(Sc& c) {
    c.fmt("%d, \"%s\", 0x%llx, 0x%llx", c.fd(0), c.a[1] ? c.str(c.a[1]) : "NULL", (unsigned long long)c.a[2],
          (unsigned long long)c.a[3]);
    return do_utimensat(c, c.fd(0), c.a[1] ? c.str(c.a[1]) : nullptr, reinterpret_cast<const lx::timespec*>(c.a[2]),
                        (int)c.a[3]);
}

int64_t sys_utimes(Sc& c) {
    c.fmt("\"%s\", 0x%llx", c.str(c.a[0]), (unsigned long long)c.a[1]);
    const auto* tv = reinterpret_cast<const lx::timeval*>(c.a[1]);
    lx::timespec ts[2];
    if (tv) {
        ts[0] = {tv[0].tv_sec, tv[0].tv_usec * 1000};
        ts[1] = {tv[1].tv_sec, tv[1].tv_usec * 1000};
    }
    return do_utimensat(c, lx::at_fdcwd, c.str(c.a[0]), tv ? ts : nullptr, 0);
}

int64_t sys_utime(Sc& c) {
    c.fmt("\"%s\", 0x%llx", c.str(c.a[0]), (unsigned long long)c.a[1]);
    const auto* ut = reinterpret_cast<const int64_t*>(c.a[1]);  // struct utimbuf { time_t actime, modtime; }
    lx::timespec ts[2];
    if (ut) {
        ts[0] = {ut[0], 0};
        ts[1] = {ut[1], 0};
    }
    return do_utimensat(c, lx::at_fdcwd, c.str(c.a[0]), ut ? ts : nullptr, 0);
}

int64_t sys_futimesat(Sc& c) {
    c.fmt("%d, \"%s\", 0x%llx", c.fd(0), c.str(c.a[1]), (unsigned long long)c.a[2]);
    const auto* tv = reinterpret_cast<const lx::timeval*>(c.a[2]);
    lx::timespec ts[2];
    if (tv) {
        ts[0] = {tv[0].tv_sec, tv[0].tv_usec * 1000};
        ts[1] = {tv[1].tv_sec, tv[1].tv_usec * 1000};
    }
    return do_utimensat(c, c.fd(0), c.a[1] ? c.str(c.a[1]) : nullptr, tv ? ts : nullptr, 0);
}

// ---- fcntl / flock ----------------------------------------------------------------------

// Converts a struct flock to [start, end) against the file's position/size.
bool flock_range(OpenFile& f, const lx::flock& fl, uint64_t& start, uint64_t& end) {
    int64_t base = 0;
    if (fl.l_whence == lx::seek_cur) {
        base = f.lseek(0, lx::seek_cur);
        if (base < 0) base = 0;
    } else if (fl.l_whence == lx::seek_end) {
        lx::stat st {};
        if (f.fstat(st) == 0) base = st.st_size;
    } else if (fl.l_whence != lx::seek_set) {
        return false;
    }
    const int64_t s = base + fl.l_start;
    if (s < 0) return false;
    start = (uint64_t)s;
    if (fl.l_len == 0) {
        end = UINT64_MAX;
    } else if (fl.l_len > 0) {
        end = start + (uint64_t)fl.l_len;
    } else {
        if ((int64_t)start + fl.l_len < 0) return false;
        end = start;
        start = (uint64_t)((int64_t)start + fl.l_len);
    }
    return end > start;
}

int64_t sys_fcntl(Sc& c) {
    c.fmt("%d, %d, 0x%llx", c.fd(0), (int)c.a[1], (unsigned long long)c.a[2]);
    const int fd = c.fd(0), cmd = (int)c.a[1];
    auto f = fd_get(c, fd);
    if (!f) return -lx::ebadf;
    switch (cmd) {
        case lx::f_dupfd: return c.p.fds.dup(fd, (int)c.a[2], false);
        case lx::f_dupfd_cloexec: return c.p.fds.dup(fd, (int)c.a[2], true);
        case lx::f_getfd: return c.p.fds.get_cloexec(fd);
        case lx::f_setfd: return c.p.fds.set_cloexec(fd, (c.a[2] & lx::fd_cloexec) != 0);
        case lx::f_getfl: return f->oflags & ~(lx::o_creat | lx::o_excl | lx::o_trunc);
        case lx::f_setfl:
            f->oflags = (f->oflags & (lx::o_accmode | lx::o_path)) | ((int)c.a[2] & (lx::o_append | lx::o_nonblock | lx::o_noatime | lx::o_dsync));
            return 0;
        case lx::f_getlk:
        case lx::f_setlk:
        case lx::f_setlkw:
        case lx::f_ofd_getlk:
        case lx::f_ofd_setlk:
        case lx::f_ofd_setlkw: {
            auto* fl = reinterpret_cast<lx::flock*>(c.a[2]);
            if (!fl) return -lx::efault;
            LockKey key;
            if (!lock_key_of(*f, key)) return -lx::ebadf;
            uint64_t start, end;
            if (!flock_range(*f, *fl, start, end)) return -lx::einval;
            const bool ofd = cmd >= lx::f_ofd_getlk;
            const void* owner = ofd ? static_cast<const void*>(f.get()) : nullptr;
            if (ofd && fl->l_pid != 0) return -lx::einval;
            if (cmd == lx::f_getlk || cmd == lx::f_ofd_getlk) {
                return LockTable::get().get(key, start, end, fl->l_type, c.p.pid, owner, *fl);
            }
            if (fl->l_type == lx::f_rdlck && (f->oflags & lx::o_accmode) == lx::o_wronly) return -lx::ebadf;
            if (fl->l_type == lx::f_wrlck && (f->oflags & lx::o_accmode) == lx::o_rdonly) return -lx::ebadf;
            const bool wait = cmd == lx::f_setlkw || cmd == lx::f_ofd_setlkw;
            int r = LockTable::get().set(key, start, end, fl->l_type, c.p.pid, owner, wait);
            if (r == 0 && fl->l_type != lx::f_unlck) f->has_locks = true;
            if (r == -lx::eintr) c.t.restartable = true;
            return r;
        }
        case lx::f_setown:
        case lx::f_setsig: return 0;
        case lx::f_getown:
        case lx::f_getsig: return 0;
        case lx::f_getpipe_sz: return (int64_t)Pipe::kCapacity;
        case lx::f_setpipe_sz: return (int64_t)Pipe::kCapacity;
        case lx::f_add_seals: return 0;
        case lx::f_get_seals: return 0;
        default: return -lx::einval;
    }
}

int64_t sys_flock(Sc& c) {
    c.fmt("%d, %d", c.fd(0), (int)c.a[1]);
    auto f = fd_get(c, c.fd(0));
    if (!f) return -lx::ebadf;
    LockKey key;
    if (!lock_key_of(*f, key)) return -lx::ebadf;
    int r = LockTable::get().flock(key, f.get(), (int)c.a[1]);
    if (r == 0 && ((int)c.a[1] & ~lx::lock_nb) != lx::lock_un) f->has_locks = true;
    if (r == -lx::eintr) c.t.restartable = true;
    return r;
}

// ---- statfs ------------------------------------------------------------------------------

void fill_statfs(uint64_t* f, uint64_t magic) {
    memset(f, 0, 120);
    f[0] = magic;  // f_type
    f[1] = 4096;   // f_bsize
    if (magic == lx::tmpfs_magic) {  // the writable layer: "1 GB, mostly free" (it is app memory)
        f[2] = 256u << 10;          // f_blocks (4 KB units)
        f[3] = f[4] = 240u << 10;   // free / avail
    } else {                        // the read-only image from the laptop: 48 GB, full
        f[2] = 12u << 20;
        f[3] = f[4] = magic == lx::proc_super_magic ? 0 : 1u << 18;
    }
    f[5] = 1u << 20;  // files
    f[6] = 1u << 19;  // ffree
    f[8] = 255;       // namelen
    f[9] = 4096;      // frsize
}

uint64_t magic_for(const std::string& path, uint64_t dev) {
    if (path == "/proc" || path.rfind("/proc/", 0) == 0) return lx::proc_super_magic;
    if (dev == 0x14) return lx::tmpfs_magic;
    return lx::ext4_super_magic;
}

int64_t sys_statfs(Sc& c) {
    c.fmt("\"%s\", 0x%llx", c.str(c.a[0]), (unsigned long long)c.a[1]);
    if (!c.a[1]) return -lx::efault;
    const std::string full = K().resolve_path(c.p, lx::at_fdcwd, c.str(c.a[0]));
    lx::stat st {};
    int err = K().stat_path(full, true, st);
    if (err) return -err;
    fill_statfs(reinterpret_cast<uint64_t*>(c.a[1]), magic_for(full, st.st_dev));
    return 0;
}

int64_t sys_fstatfs(Sc& c) {
    c.fmt("%d, 0x%llx", c.fd(0), (unsigned long long)c.a[1]);
    auto f = fd_get(c, c.fd(0));
    if (!f) return -lx::ebadf;
    if (!c.a[1]) return -lx::efault;
    lx::stat st {};
    f->fstat(st);
    fill_statfs(reinterpret_cast<uint64_t*>(c.a[1]), magic_for(f->path, st.st_dev));
    return 0;
}

// ---- memfd / sendfile ----------------------------------------------------------------

int64_t sys_memfd_create(Sc& c) {
    c.fmt("\"%s\", 0x%llx", c.str(c.a[0]), (unsigned long long)c.a[1]);
    if (!c.a[0]) return -lx::efault;
    const unsigned flags = (unsigned)c.a[1];
    if (flags & ~(lx::mfd_cloexec | lx::mfd_allow_sealing | 4u /* MFD_HUGETLB */)) return -lx::einval;
    // An anonymous upper file: no directory entry, freed with the last fd.
    auto node = std::make_shared<TmpNode>();
    node->kind = TmpNode::Reg;
    node->mode = lx::s_ifreg | 0777;
    node->ino = Overlay::new_ino();
    node->atime = node->mtime = node->ctime = Overlay::now_sec();
    node->data = std::make_shared<TmpData>();
    node->nlink = 0;
    const std::string name = std::string("/memfd:") + c.str(c.a[0]) + " (deleted)";
    return c.p.fds.alloc(std::make_shared<TmpFile>(node, name, lx::o_rdwr), flags & lx::mfd_cloexec);
}

int64_t sys_sendfile(Sc& c) {
    c.fmt("%d, %d, 0x%llx, %llu", c.fd(0), c.fd(1), (unsigned long long)c.a[2], (unsigned long long)c.a[3]);
    auto out = fd_get(c, c.fd(0));
    auto in = fd_get(c, c.fd(1));
    if (!out || !in) return -lx::ebadf;
    auto* off = reinterpret_cast<int64_t*>(c.a[2]);
    size_t count = (size_t)c.a[3];
    std::vector<uint8_t> buf(std::min<size_t>(count, 256 * 1024));
    int64_t total = 0;
    while ((size_t)total < count) {
        const size_t want = std::min(buf.size(), count - (size_t)total);
        int64_t n = off ? in->pread(buf.data(), want, (uint64_t)(*off + total)) : in->read(buf.data(), want);
        if (n < 0) return total ? total : n;
        if (n == 0) break;
        int64_t done = 0;
        while (done < n) {
            int64_t w = out->write(buf.data() + done, (size_t)(n - done));
            if (w < 0) {
                if (w == -lx::epipe) K().send_signal_thread(c.t, lx::sigpipe, nullptr);
                return total ? total : w;
            }
            done += w;
        }
        total += n;
        if ((size_t)n < want) break;
    }
    if (off) *off += total;
    return total;
}

int64_t sys_ret0_fs(Sc& c) {
    c.fmt("0x%llx, 0x%llx", (unsigned long long)c.a[0], (unsigned long long)c.a[1]);
    return 0;
}

int64_t sys_eperm(Sc& c) {
    c.fmt("0x%llx, 0x%llx", (unsigned long long)c.a[0], (unsigned long long)c.a[1]);
    return -lx::eperm;
}

}  // namespace

void register_fs_syscalls(SetFn set) {
    set(2, sys_open); set(257, sys_openat); set(85, sys_creat);
    set(21, sys_access); set(269, sys_faccessat); set(439, sys_faccessat2);
    set(83, sys_mkdir); set(258, sys_mkdirat); set(84, sys_rmdir); set(87, sys_unlink); set(263, sys_unlinkat);
    set(82, sys_rename); set(264, sys_renameat); set(316, sys_renameat2); set(86, sys_link); set(265, sys_linkat);
    set(88, sys_symlink); set(266, sys_symlinkat); set(133, sys_mknod); set(259, sys_mknodat);
    set(90, sys_chmod); set(91, sys_fchmod); set(268, sys_fchmodat); set(452, sys_fchmodat2);
    set(92, sys_chown); set(93, sys_fchown); set(94, sys_lchown); set(260, sys_fchownat);
    set(76, sys_truncate); set(77, sys_ftruncate); set(285, sys_fallocate);
    set(280, sys_utimensat); set(235, sys_utimes); set(132, sys_utime); set(261, sys_futimesat);
    set(72, sys_fcntl); set(73, sys_flock); set(137, sys_statfs); set(138, sys_fstatfs);
    set(319, sys_memfd_create); set(40, sys_sendfile);
    set(162, sys_ret0_fs /*sync*/); set(306, sys_ret0_fs /*syncfs*/); set(277, sys_ret0_fs /*sync_file_range*/);
    set(161, sys_eperm /*chroot*/); set(165, sys_eperm /*mount*/); set(166, sys_eperm /*umount2*/);
}

}  // namespace rlk
