// fds.h — open files and the per-process fd table. Every method returns
// Linux conventions: >= 0 or -errno (lx numbering).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "file_source.h"
#include "linux_abi.h"

namespace rlk {

class OpenFile {
public:
    virtual ~OpenFile() = default;
    virtual int64_t read(void* buf, size_t len) { return -lx::ebadf; }
    virtual int64_t write(const void* buf, size_t len) { return -lx::ebadf; }
    virtual int64_t pread(void* buf, size_t len, uint64_t off) { return -lx::espipe; }
    virtual int64_t pwrite(const void* buf, size_t len, uint64_t off) { return -lx::espipe; }
    virtual int64_t lseek(int64_t off, int whence) { return -lx::espipe; }
    virtual int fstat(lx::stat& st) = 0;
    virtual int64_t getdents64(void* buf, size_t cap) { return -lx::enotdir; }
    virtual int64_t ioctl(unsigned req, uint64_t arg) { return -lx::enotty; }
    virtual bool is_dir() const { return false; }
    virtual bool is_tty() const { return false; }
    // For mmap of a file: the byte source (nullptr = not mappable).
    virtual std::shared_ptr<FileSource> source() { return nullptr; }

    std::string path;   // guest path (for /proc/self/fd, openat(dirfd))
    int oflags = 0;     // lx::o_* at open
    std::mutex mu;
};

// Regular read-only file over a FileSource (network VFS or host file).
class RegularFile final : public OpenFile {
public:
    RegularFile(std::shared_ptr<FileSource> src, const lx::stat& st, std::string guest_path, int oflags);
    int64_t read(void* buf, size_t len) override;
    int64_t pread(void* buf, size_t len, uint64_t off) override;
    int64_t lseek(int64_t off, int whence) override;
    int fstat(lx::stat& st) override;
    std::shared_ptr<FileSource> source() override { return src_; }

private:
    std::shared_ptr<FileSource> src_;
    lx::stat st_;
    uint64_t pos_ = 0;
};

struct DirEntryInfo {
    std::string name;
    uint8_t dtype;
    uint64_t ino;
};

class DirFile final : public OpenFile {
public:
    DirFile(std::vector<DirEntryInfo> entries, const lx::stat& st, std::string guest_path, int oflags);
    int fstat(lx::stat& st) override;
    int64_t getdents64(void* buf, size_t cap) override;
    int64_t lseek(int64_t off, int whence) override;
    bool is_dir() const override { return true; }

private:
    std::vector<DirEntryInfo> entries_;
    lx::stat st_;
    size_t pos_ = 0;
};

// stdin/stdout/stderr of a guest process: writes go to the kernel log and
// a per-process capture; reads return EOF. Reported as a pipe (no tty).
class ConsoleFile final : public OpenFile {
public:
    using Sink = std::function<void(int fd, std::string_view data)>;
    ConsoleFile(int fdnum, Sink sink);
    int64_t read(void* buf, size_t len) override { return 0; }
    int64_t write(const void* buf, size_t len) override;
    int fstat(lx::stat& st) override;

private:
    int fdnum_;
    Sink sink_;
};

class DevNullFile final : public OpenFile {
public:
    explicit DevNullFile(int kind);  // 0 = null, 1 = zero, 2 = urandom
    int64_t read(void* buf, size_t len) override;
    int64_t write(const void* buf, size_t len) override { return (int64_t)len; }
    int64_t lseek(int64_t, int) override { return 0; }
    int fstat(lx::stat& st) override;

private:
    int kind_;
};

struct FdEntry {
    std::shared_ptr<OpenFile> file;
    bool cloexec = false;
};

class FdTable {
public:
    FdTable() = default;
    FdTable(const FdTable& o);  // dup for fork
    int alloc(std::shared_ptr<OpenFile> f, bool cloexec, int min_fd = 0);
    std::shared_ptr<OpenFile> get(int fd) const;
    int close(int fd);
    int dup(int fd, int min_fd, bool cloexec);
    int dup2(int oldfd, int newfd, bool cloexec);
    int set_cloexec(int fd, bool on);
    int get_cloexec(int fd) const;  // 0/1 or -EBADF
    void close_on_exec();
    void clear();   // process exit: drop every open file
    size_t count() const;
    std::vector<std::pair<int, std::string>> list() const;
    static constexpr int kMaxFds = 4096;

private:
    mutable std::mutex mu_;
    std::vector<FdEntry> fds_;
};

// Fills a Linux stat from the parts the VFS knows.
void FillStat(lx::stat& st, uint32_t mode, uint64_t size, int64_t mtime, uint64_t ino, uint64_t rdev = 0,
              uint32_t uid = 1000, uint32_t gid = 1000);

}  // namespace rlk
