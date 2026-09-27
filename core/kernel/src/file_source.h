// file_source.h — where the loader and the fd table read guest file bytes
// from: the network VFS on the TV, a host file or a memory buffer in tests.
#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "linux_abi.h"

namespace rlk {

class FileSource {
public:
    virtual ~FileSource() = default;
    virtual uint64_t size() const = 0;
    // Bytes read, 0 at EOF, -errno (Linux numbering) on error.
    virtual int64_t pread(void* buf, size_t len, uint64_t off) = 0;
    virtual const std::string& path() const = 0;  // guest path (for logs, /proc)
    virtual uint64_t inode() const { return 0; }
};

class MemFileSource final : public FileSource {
public:
    MemFileSource(std::string path, std::vector<uint8_t> bytes) : path_(std::move(path)), bytes_(std::move(bytes)) {}
    uint64_t size() const override { return bytes_.size(); }
    int64_t pread(void* buf, size_t len, uint64_t off) override {
        if (off >= bytes_.size()) return 0;
        size_t n = std::min<size_t>(len, bytes_.size() - off);
        memcpy(buf, bytes_.data() + off, n);
        return (int64_t)n;
    }
    const std::string& path() const override { return path_; }

private:
    std::string path_;
    std::vector<uint8_t> bytes_;
};

// Reads the whole file (small helper for headers).
inline bool ReadAll(FileSource& f, uint64_t off, void* buf, size_t len) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t done = 0;
    while (done < len) {
        int64_t n = f.pread(p + done, len - done, off + done);
        if (n <= 0) return false;
        done += (size_t)n;
    }
    return true;
}

}  // namespace rlk
