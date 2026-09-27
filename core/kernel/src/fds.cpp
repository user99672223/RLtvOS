// fds.cpp — see fds.h.
#include "fds.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "locks.h"
#include "log.h"

namespace rlk {

void FillStat(lx::stat& st, uint32_t mode, uint64_t size, int64_t mtime, uint64_t ino, uint64_t rdev, uint32_t uid,
              uint32_t gid) {
    memset(&st, 0, sizeof st);
    st.st_dev = 0x801;
    st.st_ino = ino ? ino : 1;
    st.st_nlink = 1;
    st.st_mode = mode;
    st.st_uid = uid;
    st.st_gid = gid;
    st.st_rdev = rdev;
    st.st_size = (int64_t)size;
    st.st_blksize = 4096;
    st.st_blocks = (int64_t)((size + 511) / 512);
    st.st_atim.tv_sec = st.st_mtim.tv_sec = st.st_ctim.tv_sec = mtime;
}

// ---- RegularFile ------------------------------------------------------------------

RegularFile::RegularFile(std::shared_ptr<FileSource> src, const lx::stat& st, std::string guest_path, int oflags)
    : src_(std::move(src)), st_(st) {
    path = std::move(guest_path);
    this->oflags = oflags;
}

RegularFile::~RegularFile() {
    if (has_locks) LockTable::get().release_ofd(this);  // OFD / flock locks die with the description
}

int64_t RegularFile::read(void* buf, size_t len) {
    std::lock_guard<std::mutex> lk(mu);
    if ((oflags & lx::o_accmode) == lx::o_wronly) return -lx::ebadf;
    int64_t n = src_->pread(buf, len, pos_);
    if (n > 0) pos_ += (uint64_t)n;
    return n;
}

int64_t RegularFile::pread(void* buf, size_t len, uint64_t off) {
    if ((oflags & lx::o_accmode) == lx::o_wronly) return -lx::ebadf;
    return src_->pread(buf, len, off);
}

int64_t RegularFile::lseek(int64_t off, int whence) {
    std::lock_guard<std::mutex> lk(mu);
    int64_t base = 0;
    switch (whence) {
        case lx::seek_set: base = 0; break;
        case lx::seek_cur: base = (int64_t)pos_; break;
        case lx::seek_end: base = (int64_t)src_->size(); break;
        default: return -lx::einval;
    }
    int64_t np = base + off;
    if (np < 0) return -lx::einval;
    pos_ = (uint64_t)np;
    return np;
}

int RegularFile::fstat(lx::stat& st) {
    st = st_;
    return 0;
}

// ---- DirFile -----------------------------------------------------------------------

DirFile::DirFile(std::vector<DirEntryInfo> entries, const lx::stat& st, std::string guest_path, int oflags)
    : entries_(std::move(entries)), st_(st) {
    path = std::move(guest_path);
    this->oflags = oflags;
}

int DirFile::fstat(lx::stat& st) {
    st = st_;
    return 0;
}

int64_t DirFile::lseek(int64_t off, int whence) {
    std::lock_guard<std::mutex> lk(mu);
    if (whence == lx::seek_set && off >= 0) {
        pos_ = (size_t)off;
        return off;
    }
    if (whence == lx::seek_cur && off == 0) return (int64_t)pos_;
    return -lx::einval;
}

int64_t DirFile::getdents64(void* buf, size_t cap) {
    std::lock_guard<std::mutex> lk(mu);
    uint8_t* out = static_cast<uint8_t*>(buf);
    size_t used = 0;
    constexpr size_t hdr = lx::dirent64_hdr_size;  // d_name starts at byte 19
    while (pos_ < entries_.size()) {
        const auto& e = entries_[pos_];
        size_t reclen = (hdr + e.name.size() + 1 + 7) & ~(size_t)7;
        if (used + reclen > cap) {
            if (used == 0) return -lx::einval;
            break;
        }
        lx::dirent64_hdr h;
        h.d_ino = e.ino ? e.ino : 1;
        h.d_off = (int64_t)pos_ + 1;
        h.d_reclen = (uint16_t)reclen;
        h.d_type = e.dtype;
        memcpy(out + used, &h, hdr);
        memcpy(out + used + hdr, e.name.data(), e.name.size());
        memset(out + used + hdr + e.name.size(), 0, reclen - hdr - e.name.size());
        used += reclen;
        ++pos_;
    }
    return (int64_t)used;
}

// ---- ConsoleFile --------------------------------------------------------------------

ConsoleFile::ConsoleFile(int fdnum, Sink sink) : fdnum_(fdnum), sink_(std::move(sink)) {
    path = fdnum == 0 ? "pipe:[stdin]" : (fdnum == 1 ? "pipe:[stdout]" : "pipe:[stderr]");
    oflags = fdnum == 0 ? lx::o_rdonly : lx::o_wronly;
}

int64_t ConsoleFile::write(const void* buf, size_t len) {
    if (sink_) sink_(fdnum_, std::string_view(static_cast<const char*>(buf), len));
    return (int64_t)len;
}

int ConsoleFile::fstat(lx::stat& st) {
    FillStat(st, lx::s_ififo | 0600, 0, 0, 100 + (uint64_t)fdnum_);
    return 0;
}

// ---- DevNullFile -----------------------------------------------------------------------

DevNullFile::DevNullFile(int kind) : kind_(kind) {
    path = kind == 0 ? "/dev/null" : (kind == 1 ? "/dev/zero" : "/dev/urandom");
    oflags = lx::o_rdwr;
}

int64_t DevNullFile::read(void* buf, size_t len) {
    if (kind_ == 0) return 0;
    if (kind_ == 1) {
        memset(buf, 0, len);
        return (int64_t)len;
    }
    FillRandom(buf, len);
    return (int64_t)len;
}

int DevNullFile::fstat(lx::stat& st) {
    static const uint64_t rdevs[] = {(1u << 8) | 3, (1u << 8) | 5, (1u << 8) | 9};
    FillStat(st, lx::s_ifchr | 0666, 0, 0, 50 + (uint64_t)kind_, rdevs[kind_], 0, 0);
    return 0;
}

// ---- FdTable -----------------------------------------------------------------------------

FdTable::FdTable(const FdTable& o) {
    std::lock_guard<std::mutex> lk(o.mu_);
    fds_ = o.fds_;
}

void FdTable::clone_from(const FdTable& o) {
    std::lock_guard<std::mutex> lo(o.mu_);
    std::lock_guard<std::mutex> lk(mu_);
    fds_ = o.fds_;
}

int FdTable::alloc(std::shared_ptr<OpenFile> f, bool cloexec, int min_fd) {
    std::lock_guard<std::mutex> lk(mu_);
    if (min_fd < 0) return -lx::einval;
    for (size_t i = (size_t)min_fd; i < fds_.size(); i++) {
        if (!fds_[i].file) {
            fds_[i] = {std::move(f), cloexec};
            return (int)i;
        }
    }
    size_t idx = std::max<size_t>(fds_.size(), (size_t)min_fd);
    if (idx >= (size_t)kMaxFds) return -lx::emfile;
    fds_.resize(idx + 1);
    fds_[idx] = {std::move(f), cloexec};
    return (int)idx;
}

std::shared_ptr<OpenFile> FdTable::get(int fd) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (fd < 0 || (size_t)fd >= fds_.size()) return nullptr;
    return fds_[(size_t)fd].file;
}

int FdTable::close(int fd) {
    std::lock_guard<std::mutex> lk(mu_);
    if (fd < 0 || (size_t)fd >= fds_.size() || !fds_[(size_t)fd].file) return -lx::ebadf;
    fds_[(size_t)fd] = {};
    return 0;
}

int FdTable::dup(int fd, int min_fd, bool cloexec) {
    auto f = get(fd);
    if (!f) return -lx::ebadf;
    return alloc(f, cloexec, min_fd);
}

int FdTable::dup2(int oldfd, int newfd, bool cloexec) {
    std::lock_guard<std::mutex> lk(mu_);
    if (oldfd < 0 || (size_t)oldfd >= fds_.size() || !fds_[(size_t)oldfd].file) return -lx::ebadf;
    if (newfd < 0 || newfd >= kMaxFds) return -lx::ebadf;
    if (oldfd == newfd) return newfd;
    if ((size_t)newfd >= fds_.size()) fds_.resize((size_t)newfd + 1);
    fds_[(size_t)newfd] = {fds_[(size_t)oldfd].file, cloexec};
    return newfd;
}

int FdTable::set_cloexec(int fd, bool on) {
    std::lock_guard<std::mutex> lk(mu_);
    if (fd < 0 || (size_t)fd >= fds_.size() || !fds_[(size_t)fd].file) return -lx::ebadf;
    fds_[(size_t)fd].cloexec = on;
    return 0;
}

int FdTable::get_cloexec(int fd) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (fd < 0 || (size_t)fd >= fds_.size() || !fds_[(size_t)fd].file) return -lx::ebadf;
    return fds_[(size_t)fd].cloexec ? 1 : 0;
}

void FdTable::close_on_exec() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& e : fds_) {
        if (e.cloexec) e = {};
    }
}

void FdTable::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    fds_.clear();
}

size_t FdTable::count() const {
    std::lock_guard<std::mutex> lk(mu_);
    size_t n = 0;
    for (auto& e : fds_) n += e.file ? 1 : 0;
    return n;
}

std::vector<std::pair<int, std::string>> FdTable::list() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::pair<int, std::string>> out;
    for (size_t i = 0; i < fds_.size(); i++) {
        if (fds_[i].file) out.emplace_back((int)i, fds_[i].file->path);
    }
    return out;
}

}  // namespace rlk
