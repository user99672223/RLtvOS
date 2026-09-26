// vfs.cpp — see vfs.h
#include "rlvfs/vfs.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>

namespace rlvfs {

Vfs::Vfs(const Manifest& m, BlockCache& cache) : manifest_(m), cache_(cache) {}

void Vfs::add_mount(const std::string& guest_prefix, const std::string& root) {
    std::string p = normalize(guest_prefix);
    mounts_.push_back({p, root});
    std::sort(mounts_.begin(), mounts_.end(),
              [](const Mount& a, const Mount& b) { return a.prefix.size() > b.prefix.size(); });
}

std::string Vfs::normalize(std::string_view path) {
    std::vector<std::string_view> parts;
    size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && path[i] == '/') i++;
        size_t j = i;
        while (j < path.size() && path[j] != '/') j++;
        if (j > i) {
            std::string_view c = path.substr(i, j - i);
            if (c == ".") {
            } else if (c == "..") {
                if (!parts.empty()) parts.pop_back();
            } else {
                parts.push_back(c);
            }
        }
        i = j;
    }
    std::string out;
    for (auto& c : parts) { out.push_back('/'); out.append(c); }
    if (out.empty()) out = "/";
    return out;
}

uint64_t Vfs::inode_for(std::string_view manifest_path) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : manifest_path) { h ^= c; h *= 1099511628211ULL; }
    h |= 1ULL << 62;  // keep away from small numbers used by synthetic files
    return h & 0x7fffffffffffffffULL;
}

const Vfs::Mount* Vfs::mount_for(std::string_view path, size_t* prefix_len) const {
    for (const Mount& m : mounts_) {
        if (m.prefix == "/") { *prefix_len = 0; return &m; }
        if (path == m.prefix || (path.size() > m.prefix.size() && path.compare(0, m.prefix.size(), m.prefix) == 0 &&
                                 path[m.prefix.size()] == '/')) {
            *prefix_len = m.prefix.size();
            return &m;
        }
    }
    return nullptr;
}

std::string Vfs::to_manifest_path(std::string_view canonical) const {
    size_t plen = 0;
    const Mount* m = mount_for(canonical, &plen);
    if (!m) return "";
    std::string_view rest = canonical.substr(plen);
    if (rest.empty() || rest == "/") return m->root;
    return m->root + std::string(rest);
}

const Entry* Vfs::resolve(std::string_view guest_path, bool follow_last, int* err, std::string* resolved) const {
    std::string path = normalize(guest_path);
    int hops = 0;
    for (;;) {
        // Walk component by component so intermediate symlinks are honoured.
        std::string cur = "/";
        size_t i = 1;
        const Entry* e = nullptr;
        bool restarted = false;
        while (i <= path.size()) {
            size_t j = path.find('/', i);
            if (j == std::string::npos) j = path.size();
            std::string_view comp(path.data() + i, j - i);
            std::string next = cur == "/" ? "/" + std::string(comp) : cur + "/" + std::string(comp);
            if (comp.empty()) { i = j + 1; continue; }
            std::string mp = to_manifest_path(next);
            e = mp.empty() ? nullptr : manifest_.find(mp);
            if (!e) { if (err) *err = ENOENT; return nullptr; }
            bool last = j >= path.size();
            if (e->is_symlink() && (!last || follow_last)) {
                if (++hops > 40) { if (err) *err = ELOOP; return nullptr; }
                std::string target = e->link;
                std::string rest = last ? "" : path.substr(j);  // "/remaining"
                if (!target.empty() && target[0] == '/') path = normalize(target + rest);
                else path = normalize(cur + "/" + target + rest);
                restarted = true;
                break;
            }
            if (!last && !e->is_dir()) { if (err) *err = ENOTDIR; return nullptr; }
            cur = next;
            i = j + 1;
        }
        if (restarted) continue;
        if (path == "/") {
            std::string mp = to_manifest_path("/");
            e = manifest_.find(mp);
            if (!e) { if (err) *err = ENOENT; return nullptr; }
        }
        if (resolved) *resolved = path;
        if (err) *err = 0;
        return e;
    }
}

void Vfs::stat_entry(const Entry& e, GuestStat& out) {
    uint32_t type = 0;
    switch (e.type) {
        case EntryType::File: type = S_IFREG; break;
        case EntryType::Dir: type = S_IFDIR; break;
        case EntryType::Symlink: type = S_IFLNK; break;
        case EntryType::CharDev: type = S_IFCHR; break;
        case EntryType::BlockDev: type = S_IFBLK; break;
        case EntryType::Fifo: type = S_IFIFO; break;
        case EntryType::Socket: type = S_IFSOCK; break;
    }
    out.mode = type | (e.mode ? e.mode : (e.is_dir() ? 0755u : 0644u));
    out.size = e.is_symlink() ? e.link.size() : e.size;
    out.mtime = e.mtime;
    out.ino = inode_for(e.path);
    out.rdev = e.rdev;
    out.nlink = e.is_dir() ? (uint32_t)(2 + e.children.size()) : 1;
    out.uid = 0;
    out.gid = 0;
}

int Vfs::stat(std::string_view guest_path, bool follow, GuestStat& out) const {
    int err = 0;
    const Entry* e = resolve(guest_path, follow, &err);
    if (!e) return -err;
    stat_entry(*e, out);
    return 0;
}

int Vfs::readlink(std::string_view guest_path, std::string& target) const {
    int err = 0;
    const Entry* e = resolve(guest_path, false, &err);
    if (!e) return -err;
    if (!e->is_symlink()) return -EINVAL;
    target = e->link;
    return 0;
}

int Vfs::readdir(std::string_view guest_path, std::vector<DirEnt>& out) const {
    int err = 0;
    std::string canon;
    const Entry* e = resolve(guest_path, true, &err, &canon);
    if (!e) return -err;
    if (!e->is_dir()) return -ENOTDIR;
    out.clear();
    out.push_back({".", DT_DIR, inode_for(e->path)});
    out.push_back({"..", DT_DIR, e->parent ? inode_for(e->parent->path) : inode_for(e->path)});
    // Mount points shadow directories of the parent root.
    std::vector<std::string_view> shadow;
    for (const Mount& m : mounts_) {
        if (m.prefix == "/" || m.prefix == canon) continue;
        std::string_view rest(m.prefix);
        if (canon != "/" && !(rest.size() > canon.size() && rest.compare(0, canon.size(), canon) == 0 && rest[canon.size()] == '/'))
            continue;
        rest.remove_prefix(canon == "/" ? 1 : canon.size() + 1);
        if (rest.find('/') != std::string_view::npos) continue;  // deeper mount
        shadow.push_back(rest);
    }
    for (const Entry* c : e->children) {
        uint8_t dt = DT_UNKNOWN;
        switch (c->type) {
            case EntryType::File: dt = DT_REG; break;
            case EntryType::Dir: dt = DT_DIR; break;
            case EntryType::Symlink: dt = DT_LNK; break;
            case EntryType::CharDev: dt = DT_CHR; break;
            case EntryType::BlockDev: dt = DT_BLK; break;
            case EntryType::Fifo: dt = DT_FIFO; break;
            case EntryType::Socket: dt = DT_SOCK; break;
        }
        if (std::find(shadow.begin(), shadow.end(), c->name) != shadow.end()) dt = DT_DIR;
        out.push_back({std::string(c->name), dt, inode_for(c->path)});
    }
    for (auto s : shadow) {
        bool present = false;
        for (auto& d : out) if (d.name == s) { present = true; break; }
        if (!present) out.push_back({std::string(s), DT_DIR, inode_for(std::string(canon == "/" ? "" : canon) + "/" + std::string(s))});
    }
    return 0;
}

int64_t Vfs::pread(const Entry& e, void* buf, size_t len, uint64_t off, std::string& err) {
    return cache_.pread(e, buf, len, off, err);
}

}  // namespace rlvfs
