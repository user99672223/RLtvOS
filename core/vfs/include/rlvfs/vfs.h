// vfs.h — guest-visible read-only namespace over the manifest roots:
//   /            → rootfs      /prefix     → prefix
//   /game        → game        /home/user  → home
// Symlinks are resolved inside the guest namespace (absolute targets restart
// at "/"). Writable overlay comes later (Phase C).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rlvfs/blockcache.h"
#include "rlvfs/manifest.h"

namespace rlvfs {

struct GuestStat {
    uint32_t mode = 0;       // type bits | permissions (S_IFREG etc.)
    uint64_t size = 0;
    int64_t mtime = 0;
    uint64_t ino = 0;
    uint64_t rdev = 0;
    uint32_t nlink = 1;
    uint32_t uid = 0;
    uint32_t gid = 0;
};

struct DirEnt {
    std::string name;
    uint8_t dtype;  // DT_REG, DT_DIR, DT_LNK, ...
    uint64_t ino;
};

class Vfs {
public:
    Vfs(const Manifest& m, BlockCache& cache);

    // guest_prefix "/" or "/prefix" (no trailing slash except root).
    void add_mount(const std::string& guest_prefix, const std::string& root);

    // Resolve a guest absolute path. follow_last: follow a final symlink.
    // Returns the entry or nullptr with *err = ENOENT/ENOTDIR/ELOOP.
    // resolved (optional) receives the canonical guest path.
    const Entry* resolve(std::string_view guest_path, bool follow_last, int* err,
                         std::string* resolved = nullptr) const;

    int stat(std::string_view guest_path, bool follow, GuestStat& out) const;
    static void stat_entry(const Entry& e, GuestStat& out);
    int readlink(std::string_view guest_path, std::string& target) const;
    int readdir(std::string_view guest_path, std::vector<DirEnt>& out) const;
    // Read from a resolved File entry.
    int64_t pread(const Entry& e, void* buf, size_t len, uint64_t off, std::string& err);

    // Guest path → manifest path ("/usr/bin/ls" → "rootfs/usr/bin/ls"); "" if no mount.
    std::string to_manifest_path(std::string_view canonical_guest_path) const;
    static std::string normalize(std::string_view path);
    static uint64_t inode_for(std::string_view manifest_path);

private:
    struct Mount { std::string prefix; std::string root; };
    const Mount* mount_for(std::string_view path, size_t* prefix_len) const;

    const Manifest& manifest_;
    BlockCache& cache_;
    std::vector<Mount> mounts_;  // sorted longest prefix first
};

}  // namespace rlvfs
