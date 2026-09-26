// blockcache.h — 1 MB block cache for remote files, persisted as sparse
// files in the app's Caches directory (purgeable: every file may vanish
// between runs and is re-fetched on demand). Read-ahead fetches contiguous
// missing blocks in one Range request.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "rlvfs/http.h"
#include "rlvfs/manifest.h"

namespace rlvfs {

class BlockCache {
public:
    struct Stats {
        uint64_t reads = 0;
        uint64_t hit_blocks = 0;
        uint64_t miss_blocks = 0;
        uint64_t fetched_bytes = 0;
        uint64_t fetch_requests = 0;
        uint64_t fetch_errors = 0;
        uint64_t files_open = 0;
    };

    // cache_dir must exist or be creatable. block_size is a power of two.
    BlockCache(std::string cache_dir, HttpClient& client, size_t block_size = 1 << 20,
               size_t readahead_blocks = 4);
    ~BlockCache();

    // Reads up to len bytes at off from the remote file `e` (a manifest File
    // entry). Returns bytes read (0 at EOF) or -errno; err gets details.
    int64_t pread(const Entry& e, void* buf, size_t len, uint64_t off, std::string& err);

    // Fetch [off, off+len) into the cache without copying out.
    bool prefetch(const Entry& e, uint64_t off, uint64_t len, std::string& err);

    // Drop in-memory state (closes fds); cache files stay.
    void close_all();

    Stats stats() const;
    size_t block_size() const { return block_size_; }

private:
    struct FileState {
        std::string identity;          // "size:mtime:hash"
        std::string data_path;
        std::string map_path;
        int fd = -1;
        uint64_t size = 0;
        uint64_t nblocks = 0;
        std::vector<uint8_t> present;  // bitmap
        bool map_dirty = false;
        std::mutex mu;
        bool has(uint64_t b) const { return (present[b >> 3] >> (b & 7)) & 1; }
        void set(uint64_t b) { present[b >> 3] |= (uint8_t)(1u << (b & 7)); }
    };

    FileState* open_state(const Entry& e, std::string& err);
    bool ensure_blocks(FileState& fs, const Entry& e, uint64_t first, uint64_t last, std::string& err);
    bool fetch_run(FileState& fs, const Entry& e, uint64_t first, uint64_t count, std::string& err);
    void save_map(FileState& fs);
    std::string cache_name(const Entry& e) const;

    std::string cache_dir_;
    HttpClient& client_;
    size_t block_size_;
    size_t readahead_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::unique_ptr<FileState>> files_;
    Stats stats_;
};

// Stable 128-bit-ish hash of a string as 32 hex chars (for cache names).
std::string stable_hash_hex(std::string_view s);

}  // namespace rlvfs
