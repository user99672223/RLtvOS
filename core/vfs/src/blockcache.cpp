// blockcache.cpp — see blockcache.h
#include "rlvfs/blockcache.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace rlvfs {

std::string stable_hash_hex(std::string_view s) {
    // FNV-1a 64 + a second independent 64-bit mix → 32 hex chars.
    uint64_t h1 = 1469598103934665603ULL;
    uint64_t h2 = 0x9E3779B97F4A7C15ULL;
    for (unsigned char c : s) {
        h1 ^= c;
        h1 *= 1099511628211ULL;
        h2 = (h2 ^ c) * 0xBF58476D1CE4E5B9ULL;
        h2 ^= h2 >> 31;
    }
    char buf[40];
    snprintf(buf, sizeof buf, "%016llx%016llx", (unsigned long long)h1, (unsigned long long)h2);
    return buf;
}

BlockCache::BlockCache(std::string cache_dir, HttpClient& client, size_t block_size, size_t readahead_blocks)
    : cache_dir_(std::move(cache_dir)), client_(client), block_size_(block_size), readahead_(readahead_blocks) {
    ::mkdir(cache_dir_.c_str(), 0755);
}

BlockCache::~BlockCache() { close_all(); }

void BlockCache::close_all() {
    std::lock_guard<std::mutex> g(mu_);
    for (auto& kv : files_) {
        FileState& fs = *kv.second;
        std::lock_guard<std::mutex> fg(fs.mu);
        if (fs.map_dirty) save_map(fs);
        if (fs.fd >= 0) ::close(fs.fd);
        fs.fd = -1;
    }
    files_.clear();
    stats_.files_open = 0;
}

BlockCache::Stats BlockCache::stats() const {
    std::lock_guard<std::mutex> g(mu_);
    return stats_;
}

std::string BlockCache::cache_name(const Entry& e) const { return stable_hash_hex(e.path); }

void BlockCache::save_map(FileState& fs) {
    std::string tmp = fs.map_path + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    std::string hdr = fs.identity + "\n";
    bool ok = ::write(fd, hdr.data(), hdr.size()) == (ssize_t)hdr.size() &&
              (fs.present.empty() || ::write(fd, fs.present.data(), fs.present.size()) == (ssize_t)fs.present.size());
    ::close(fd);
    if (ok) ::rename(tmp.c_str(), fs.map_path.c_str());
    else ::unlink(tmp.c_str());
    fs.map_dirty = false;
}

BlockCache::FileState* BlockCache::open_state(const Entry& e, std::string& err) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = files_.find(e.path);
    if (it != files_.end()) {
        // Purgeable Caches: the data file may have been deleted under us.
        struct stat st;
        if (::fstat(it->second->fd, &st) == 0 && st.st_nlink > 0) return it->second.get();
        if (it->second->fd >= 0) ::close(it->second->fd);
        files_.erase(it);
        stats_.files_open--;
    }
    auto fs = std::make_unique<FileState>();
    char id[160];
    snprintf(id, sizeof id, "%llu:%lld:%s", (unsigned long long)e.size, (long long)e.mtime,
             e.hash.empty() ? "-" : e.hash.c_str());
    fs->identity = id;
    std::string base = cache_dir_ + "/" + cache_name(e);
    fs->data_path = base + ".data";
    fs->map_path = base + ".map";
    fs->size = e.size;
    fs->nblocks = (e.size + block_size_ - 1) / block_size_;
    fs->present.assign((fs->nblocks + 7) / 8, 0);

    bool fresh = true;
    FILE* mf = ::fopen(fs->map_path.c_str(), "rb");
    if (mf) {
        char line[192];
        if (fgets(line, sizeof line, mf)) {
            size_t n = strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
            if (fs->identity == line) {
                size_t got = fread(fs->present.data(), 1, fs->present.size(), mf);
                if (got == fs->present.size()) fresh = false;
            }
        }
        fclose(mf);
    }
    struct stat dst;
    if (!fresh && (::stat(fs->data_path.c_str(), &dst) != 0 || (uint64_t)dst.st_size < e.size)) fresh = true;
    int flags = O_RDWR | O_CREAT | (fresh ? O_TRUNC : 0);
    fs->fd = ::open(fs->data_path.c_str(), flags, 0644);
    if (fs->fd < 0) { err = std::string("open cache: ") + strerror(errno); return nullptr; }
    if (fresh) {
        std::fill(fs->present.begin(), fs->present.end(), 0);
        if (e.size > 0 && ::ftruncate(fs->fd, (off_t)e.size) != 0) {
            err = std::string("ftruncate cache: ") + strerror(errno);
            ::close(fs->fd);
            return nullptr;
        }
        fs->map_dirty = true;
        save_map(*fs);
    }
    FileState* raw = fs.get();
    files_[e.path] = std::move(fs);
    stats_.files_open++;
    return raw;
}

bool BlockCache::fetch_run(FileState& fs, const Entry& e, uint64_t first, uint64_t count, std::string& err) {
    uint64_t off = first * block_size_;
    uint64_t len = std::min<uint64_t>(count * block_size_, fs.size - off);
    HttpResponse resp;
    std::string path = "/f/" + url_encode_path(e.path);
    {
        std::lock_guard<std::mutex> g(mu_);
        stats_.fetch_requests++;
    }
    if (!client_.get(path, resp, off, len)) {
        std::lock_guard<std::mutex> g(mu_);
        stats_.fetch_errors++;
        err = "fetch " + e.path + ": " + resp.error;
        return false;
    }
    if (resp.status != 206 && resp.status != 200) {
        std::lock_guard<std::mutex> g(mu_);
        stats_.fetch_errors++;
        err = "fetch " + e.path + ": HTTP " + std::to_string(resp.status);
        return false;
    }
    if (resp.status == 200 && resp.body.size() != fs.size) {
        err = "fetch " + e.path + ": server ignored Range";
        return false;
    }
    const uint8_t* src = resp.body.data();
    size_t have = resp.body.size();
    if (resp.status == 200) { src += off; have = (size_t)len; }
    if (have < len) { err = "fetch " + e.path + ": short body"; return false; }
    size_t written = 0;
    while (written < len) {
        ssize_t w = ::pwrite(fs.fd, src + written, len - written, (off_t)(off + written));
        if (w < 0) {
            if (errno == EINTR) continue;
            err = std::string("pwrite cache: ") + strerror(errno);
            return false;
        }
        written += (size_t)w;
    }
    for (uint64_t b = first; b < first + count && b < fs.nblocks; b++) fs.set(b);
    fs.map_dirty = true;
    save_map(fs);
    {
        std::lock_guard<std::mutex> g(mu_);
        stats_.fetched_bytes += len;
        stats_.miss_blocks += count;
    }
    return true;
}

bool BlockCache::ensure_blocks(FileState& fs, const Entry& e, uint64_t first, uint64_t last, std::string& err) {
    uint64_t b = first;
    while (b <= last) {
        if (fs.has(b)) { b++; continue; }
        // run of missing blocks starting at b, extended by read-ahead
        uint64_t run_end = b;
        while (run_end + 1 <= last && !fs.has(run_end + 1)) run_end++;
        uint64_t want_end = std::min<uint64_t>(std::max(run_end, b + readahead_ - 1), fs.nblocks - 1);
        while (want_end > run_end && fs.has(want_end)) want_end--;
        if (!fetch_run(fs, e, b, want_end - b + 1, err)) return false;
        b = want_end + 1;
    }
    return true;
}

bool BlockCache::prefetch(const Entry& e, uint64_t off, uint64_t len, std::string& err) {
    if (!e.is_file() || e.size == 0 || off >= e.size) return true;
    FileState* fs = open_state(e, err);
    if (!fs) return false;
    std::lock_guard<std::mutex> g(fs->mu);
    uint64_t end = std::min<uint64_t>(off + len, e.size) - 1;
    return ensure_blocks(*fs, e, off / block_size_, end / block_size_, err);
}

int64_t BlockCache::pread(const Entry& e, void* buf, size_t len, uint64_t off, std::string& err) {
    if (!e.is_file()) { err = "not a regular file"; return -EISDIR; }
    if (off >= e.size || len == 0) return 0;
    if (off + len > e.size) len = (size_t)(e.size - off);
    FileState* fs = open_state(e, err);
    if (!fs) return -EIO;
    std::lock_guard<std::mutex> g(fs->mu);
    uint64_t first = off / block_size_, last = (off + len - 1) / block_size_;
    {
        std::lock_guard<std::mutex> sg(mu_);
        stats_.reads++;
        for (uint64_t b = first; b <= last; b++) if (fs->has(b)) stats_.hit_blocks++;
    }
    if (!ensure_blocks(*fs, e, first, last, err)) return -EIO;
    size_t got = 0;
    while (got < len) {
        ssize_t r = ::pread(fs->fd, (char*)buf + got, len - got, (off_t)(off + got));
        if (r < 0) {
            if (errno == EINTR) continue;
            err = std::string("pread cache: ") + strerror(errno);
            return -EIO;
        }
        if (r == 0) break;
        got += (size_t)r;
    }
    return (int64_t)got;
}

}  // namespace rlvfs
