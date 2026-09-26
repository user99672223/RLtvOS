// manifest.h — in-memory tree of the assets server's manifest.jsonl.gz.
// One line per entry: {"p":"rootfs/usr/bin/ls","t":"f","s":142144,"m":493,
// "mt":1700000000,"h":"<sha256>"} ; t: f d l(+"l") c/b(+"rdev") p s.
// Directories precede their children; missing parents are synthesised.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rlvfs {

enum class EntryType : uint8_t { File, Dir, Symlink, CharDev, BlockDev, Fifo, Socket };

struct Entry {
    std::string path;              // manifest key: "<root>/<rel>" or "<root>"
    std::string_view name;         // last component (view into path)
    EntryType type = EntryType::File;
    uint32_t mode = 0;             // permission bits only
    uint64_t size = 0;
    int64_t mtime = 0;
    uint64_t rdev = 0;
    std::string hash;              // sha256 hex or ""
    std::string link;              // symlink target
    Entry* parent = nullptr;
    std::vector<Entry*> children;  // dirs only, sorted by name

    bool is_dir() const { return type == EntryType::Dir; }
    bool is_file() const { return type == EntryType::File; }
    bool is_symlink() const { return type == EntryType::Symlink; }
    const Entry* child(std::string_view n) const;
};

class Manifest {
public:
    Manifest() = default;
    Manifest(const Manifest&) = delete;
    Manifest& operator=(const Manifest&) = delete;

    // Load from gzip-compressed JSON lines (as served at /manifest.jsonl.gz).
    bool load_gzip(const uint8_t* data, size_t n, std::string& err);
    // Load from plain JSON lines.
    bool load_text(std::string_view text, std::string& err);

    // Exact lookup by manifest path ("rootfs/usr/bin/ls" or "rootfs").
    const Entry* find(std::string_view path) const;
    size_t size() const { return entries_.size(); }
    uint64_t total_bytes() const { return total_bytes_; }
    uint64_t file_count() const { return file_count_; }
    const std::vector<std::string>& roots() const { return roots_; }

private:
    Entry* add(std::string path, EntryType type);
    Entry* ensure_dir(std::string_view path);
    bool parse_line(std::string_view line, std::string& err);

    std::deque<Entry> entries_;
    std::unordered_map<std::string_view, Entry*> by_path_;
    std::vector<std::string> roots_;
    uint64_t total_bytes_ = 0;
    uint64_t file_count_ = 0;
    size_t line_no_ = 0;
};

// Inflate a gzip/zlib stream (zlib). Returns false on error.
bool gunzip(const uint8_t* data, size_t n, std::string& out, std::string& err);

}  // namespace rlvfs
