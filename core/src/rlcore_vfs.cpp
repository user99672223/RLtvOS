// rlcore_vfs.cpp — C API over rlvfs for the app (manual testing through
// POST /run: vfs-mount, vfs-stat, vfs-ls, vfs-cat) and later the fake kernel.
#include <sys/stat.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include "rlcore/rlcore.h"
#include "rlvfs/blockcache.h"
#include "rlvfs/http.h"
#include "rlvfs/manifest.h"
#include "rlvfs/vfs.h"

namespace {

struct VfsState {
    std::string base_url, host;
    uint16_t port = 0;
    std::unique_ptr<rlvfs::HttpClient> http;
    std::unique_ptr<rlvfs::Manifest> manifest;
    std::unique_ptr<rlvfs::BlockCache> cache;
    std::unique_ptr<rlvfs::Vfs> vfs;
    std::string cache_dir;
    double mount_ms = 0;
    bool mounted = false;
};

std::mutex g_mu;
VfsState g_v;

std::string json_escape(const std::string& s) {
    static const char hex[] = "0123456789abcdef";
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20 || c >= 0x7f) {
                    o += "\\u00";
                    o += hex[c >> 4];
                    o += hex[c & 15];
                } else {
                    o += (char)c;
                }
        }
    }
    return o;
}

bool parse_url(const std::string& url, std::string& host, uint16_t& port) {
    std::string u = url;
    if (u.rfind("http://", 0) == 0) u = u.substr(7);
    size_t slash = u.find('/');
    if (slash != std::string::npos) u = u.substr(0, slash);
    size_t colon = u.rfind(':');
    if (colon == std::string::npos) { host = u; port = 80; return !host.empty(); }
    host = u.substr(0, colon);
    port = (uint16_t)atoi(u.c_str() + colon + 1);
    return !host.empty() && port != 0;
}

const char* type_name(const rlvfs::Entry& e) {
    switch (e.type) {
        case rlvfs::EntryType::File: return "file";
        case rlvfs::EntryType::Dir: return "dir";
        case rlvfs::EntryType::Symlink: return "symlink";
        case rlvfs::EntryType::CharDev: return "chr";
        case rlvfs::EntryType::BlockDev: return "blk";
        case rlvfs::EntryType::Fifo: return "fifo";
        case rlvfs::EntryType::Socket: return "sock";
    }
    return "?";
}

int fail(char* out, size_t cap, const std::string& msg) {
    snprintf(out, cap, "{\"ok\":false,\"error\":\"%s\"}", json_escape(msg).c_str());
    return 0;
}

}  // namespace

extern "C" int rlcore_vfs_mount(const char* base_url, const char* cache_dir, char* out, size_t cap) {
    std::lock_guard<std::mutex> g(g_mu);
    auto t0 = std::chrono::steady_clock::now();
    std::string host;
    uint16_t port = 0;
    if (!base_url || !parse_url(base_url, host, port)) return fail(out, cap, "bad url (want http://host:port)");
    auto http = std::make_unique<rlvfs::HttpClient>(host, port, 20000);
    rlvfs::HttpResponse resp;
    if (!http->get("/health", resp) || resp.status != 200)
        return fail(out, cap, "health: " + (resp.error.empty() ? "HTTP " + std::to_string(resp.status) : resp.error));
    if (!http->get("/manifest.jsonl.gz", resp) || resp.status != 200)
        return fail(out, cap, "manifest: " + (resp.error.empty() ? "HTTP " + std::to_string(resp.status) : resp.error));
    size_t gz_bytes = resp.body.size();
    auto manifest = std::make_unique<rlvfs::Manifest>();
    std::string err;
    if (!manifest->load_gzip(resp.body.data(), resp.body.size(), err)) return fail(out, cap, "manifest parse: " + err);
    std::string cdir = cache_dir ? cache_dir : "/tmp/rlvfs-cache";
    ::mkdir(cdir.c_str(), 0755);
    auto cache = std::make_unique<rlvfs::BlockCache>(cdir, *http, 1 << 20, 4);
    auto vfs = std::make_unique<rlvfs::Vfs>(*manifest, *cache);
    vfs->add_mount("/", "rootfs");
    vfs->add_mount("/prefix", "prefix");
    vfs->add_mount("/game", "game");
    vfs->add_mount("/home/user", "home");
    g_v.vfs.reset();
    g_v.cache.reset();
    g_v.manifest = std::move(manifest);
    g_v.http = std::move(http);
    g_v.cache = std::move(cache);
    g_v.vfs = std::move(vfs);
    g_v.base_url = base_url;
    g_v.host = host;
    g_v.port = port;
    g_v.cache_dir = cdir;
    g_v.mounted = true;
    g_v.mount_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::string roots;
    for (auto& r : g_v.manifest->roots()) { if (!roots.empty()) roots += ","; roots += "\"" + json_escape(r) + "\""; }
    snprintf(out, cap,
             "{\"ok\":true,\"url\":\"%s\",\"entries\":%zu,\"files\":%llu,\"bytes\":%llu,\"manifest_gz_bytes\":%zu,"
             "\"roots\":[%s],\"cache_dir\":\"%s\",\"ms\":%.1f}",
             json_escape(g_v.base_url).c_str(), g_v.manifest->size(), (unsigned long long)g_v.manifest->file_count(),
             (unsigned long long)g_v.manifest->total_bytes(), gz_bytes, roots.c_str(), json_escape(cdir).c_str(),
             g_v.mount_ms);
    return 1;
}

extern "C" int rlcore_vfs_stat(const char* guest_path, int follow, char* out, size_t cap) {
    std::lock_guard<std::mutex> g(g_mu);
    if (!g_v.mounted) return fail(out, cap, "not mounted (vfs-mount http://laptop:8090 first)");
    int err = 0;
    std::string canon;
    const rlvfs::Entry* e = g_v.vfs->resolve(guest_path ? guest_path : "/", follow != 0, &err, &canon);
    if (!e) {
        snprintf(out, cap, "{\"ok\":false,\"errno\":%d,\"error\":\"%s\"}", err, strerror(err));
        return 0;
    }
    rlvfs::GuestStat st;
    rlvfs::Vfs::stat_entry(*e, st);
    snprintf(out, cap,
             "{\"ok\":true,\"path\":\"%s\",\"manifest_path\":\"%s\",\"type\":\"%s\",\"mode\":\"%o\",\"size\":%llu,"
             "\"mtime\":%lld,\"ino\":%llu,\"link\":\"%s\",\"children\":%zu}",
             json_escape(canon).c_str(), json_escape(e->path).c_str(), type_name(*e), (unsigned)st.mode,
             (unsigned long long)st.size, (long long)st.mtime, (unsigned long long)st.ino,
             json_escape(e->link).c_str(), e->children.size());
    return 1;
}

extern "C" int rlcore_vfs_ls(const char* guest_path, char* out, size_t cap) {
    std::lock_guard<std::mutex> g(g_mu);
    if (!g_v.mounted) return fail(out, cap, "not mounted");
    std::vector<rlvfs::DirEnt> ents;
    int rc = g_v.vfs->readdir(guest_path ? guest_path : "/", ents);
    if (rc < 0) {
        snprintf(out, cap, "{\"ok\":false,\"errno\":%d,\"error\":\"%s\"}", -rc, strerror(-rc));
        return 0;
    }
    std::string list;
    size_t n = 0;
    for (auto& d : ents) {
        if (d.name == "." || d.name == "..") continue;
        if (n++) list += ",";
        list += "{\"name\":\"" + json_escape(d.name) + "\",\"dtype\":" + std::to_string(d.dtype) + "}";
        if (list.size() > cap - 256) { list += ",{\"name\":\"...\",\"dtype\":0}"; break; }
    }
    snprintf(out, cap, "{\"ok\":true,\"count\":%zu,\"entries\":[%s]}", ents.size() - 2, list.c_str());
    return 1;
}

extern "C" int rlcore_vfs_read(const char* guest_path, uint64_t off, uint32_t len, char* out, size_t cap) {
    std::lock_guard<std::mutex> g(g_mu);
    if (!g_v.mounted) return fail(out, cap, "not mounted");
    int err = 0;
    const rlvfs::Entry* e = g_v.vfs->resolve(guest_path ? guest_path : "/", true, &err);
    if (!e) { snprintf(out, cap, "{\"ok\":false,\"errno\":%d,\"error\":\"%s\"}", err, strerror(err)); return 0; }
    if (len > 65536) len = 65536;
    std::string buf(len, '\0');
    std::string rerr;
    auto t0 = std::chrono::steady_clock::now();
    int64_t n = g_v.vfs->pread(*e, buf.data(), len, off, rerr);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (n < 0) { snprintf(out, cap, "{\"ok\":false,\"errno\":%lld,\"error\":\"%s\"}", (long long)-n, json_escape(rerr).c_str()); return 0; }
    buf.resize((size_t)n);
    // text (escaped) is truncated to what fits; hex of the first 32 bytes always present
    std::string hex;
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < buf.size() && i < 32; i++) { hex += hx[(unsigned char)buf[i] >> 4]; hex += hx[(unsigned char)buf[i] & 15]; }
    std::string text = json_escape(buf);
    if (text.size() > cap - 512) text = text.substr(0, cap - 512);
    snprintf(out, cap, "{\"ok\":true,\"n\":%lld,\"off\":%llu,\"ms\":%.1f,\"hex32\":\"%s\",\"text\":\"%s\"}",
             (long long)n, (unsigned long long)off, ms, hex.c_str(), text.c_str());
    return 1;
}

extern "C" void* rlcore_vfs_handle(void) {
    std::lock_guard<std::mutex> g(g_mu);
    return g_v.mounted ? static_cast<void*>(g_v.vfs.get()) : nullptr;
}

extern "C" void rlcore_vfs_stats(char* out, size_t cap) {
    std::lock_guard<std::mutex> g(g_mu);
    if (!g_v.mounted) { snprintf(out, cap, "{\"mounted\":false}"); return; }
    auto s = g_v.cache->stats();
    snprintf(out, cap,
             "{\"mounted\":true,\"url\":\"%s\",\"entries\":%zu,\"files\":%llu,\"bytes\":%llu,\"mount_ms\":%.1f,"
             "\"cache\":{\"reads\":%llu,\"hit_blocks\":%llu,\"miss_blocks\":%llu,\"fetched_bytes\":%llu,"
             "\"fetch_requests\":%llu,\"fetch_errors\":%llu,\"files_open\":%llu,\"block_size\":%zu,\"dir\":\"%s\"},"
             "\"http\":{\"requests\":%llu,\"bytes\":%llu}}",
             json_escape(g_v.base_url).c_str(), g_v.manifest->size(), (unsigned long long)g_v.manifest->file_count(),
             (unsigned long long)g_v.manifest->total_bytes(), g_v.mount_ms, (unsigned long long)s.reads,
             (unsigned long long)s.hit_blocks, (unsigned long long)s.miss_blocks, (unsigned long long)s.fetched_bytes,
             (unsigned long long)s.fetch_requests, (unsigned long long)s.fetch_errors, (unsigned long long)s.files_open,
             g_v.cache->block_size(), json_escape(g_v.cache_dir).c_str(), (unsigned long long)g_v.http->requests(),
             (unsigned long long)g_v.http->bytes_received());
}
