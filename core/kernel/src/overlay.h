// overlay.h — the writable half of the guest filesystem. An in-memory upper
// layer with tmpfs semantics sits over the read-only lower tree (rlvfs from
// the laptop plus the synthetic /proc and /dev): lookups take the upper node
// when there is one, fall through to the lower tree otherwise, directory
// listings merge the two, and every mutation happens in the upper layer —
// a new file is an upper node, modifying a lower file copies it up first,
// deleting a lower name leaves a whiteout. /tmp, /dev/shm, /run and /var/tmp
// are upper-only ("opaque") directories created at boot. Nothing persists:
// the upper layer lives as long as the app (DECISIONS 2026-09-27).
//
// Paths are absolute and lexically normalized (Kernel::resolve_path);
// symlinks in either layer are followed here, component by component, so a
// lower symlink (/var/run → /run) can lead into the upper layer.
// Errors are positive Linux errno values, like Kernel::stat_path.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "fds.h"
#include "file_source.h"
#include "linux_abi.h"

namespace rlk {

class UnixSocket;
class Pipe;

// Page-granular file storage that can be aliased into guest address space:
// MAP_SHARED mappings of an upper file are host mappings of the same pages
// (Linux: a memfd mapped twice; Darwin: vm_remap of the base mapping), so a
// wineserver and its clients see one another's writes. The capacity is
// fixed for the store's lifetime (aliases must stay valid).
class SharedStore {
public:
    static std::unique_ptr<SharedStore> Create(size_t capacity);  // nullptr on failure
    ~SharedStore();
    SharedStore(const SharedStore&) = delete;
    SharedStore& operator=(const SharedStore&) = delete;
    uint8_t* base() const { return base_; }
    size_t capacity() const { return cap_; }
    // Maps [off, off+len) of the store at host address addr with protection
    // hprot (host::kProt*); addr, len and off must be host-page aligned.
    bool alias(uint64_t addr, size_t len, size_t off, int hprot);

private:
    SharedStore() = default;
    uint8_t* base_ = nullptr;
    size_t cap_ = 0;
    int fd_ = -1;  // Linux: the memfd
};

// Contents of an upper regular file (shared by hard links and open files):
// a vector until the file is mapped MAP_SHARED, a SharedStore afterwards.
// All access under mu.
struct TmpData {
    mutable std::mutex mu;
    size_t size() const { return store ? store_size : bytes.size(); }
    uint8_t* data() { return store ? store->base() : bytes.data(); }
    const uint8_t* data() const { return store ? store->base() : bytes.data(); }
    // Grows (zero-filled) or shrinks; false when a store cannot hold it.
    bool resize(size_t n);
    // Moves the contents into a store of at least min_capacity bytes (once);
    // false when the existing store is too small or memory ran out.
    bool ensure_store(size_t min_capacity);

    std::vector<uint8_t> bytes;
    std::unique_ptr<SharedStore> store;
    size_t store_size = 0;
};

struct TmpNode {
    enum Kind : uint8_t { Dir, Reg, Lnk, Sock, Fifo, Whiteout };
    Kind kind = Reg;
    uint32_t mode = 0;                 // lx::s_if* | permission bits
    uint32_t uid = 1000, gid = 1000;
    uint32_t nlink = 1;
    uint64_t ino = 0;
    int64_t atime = 0, mtime = 0, ctime = 0;   // seconds
    bool opaque = false;               // Dir: hides the lower directory of the same path
    std::string target;                // Lnk
    std::shared_ptr<TmpData> data;     // Reg
    std::weak_ptr<UnixSocket> socket;  // Sock: the socket bound at this path
    std::shared_ptr<Pipe> fifo;        // Fifo
    std::map<std::string, std::shared_ptr<TmpNode>> children;  // Dir (whiteouts included)

    uint64_t size() const;
    bool is_dir() const { return kind == Dir; }
};

// The read-only tree underneath (rlkernel.cpp: rlvfs + /proc + /dev).
class LowerFs {
public:
    virtual ~LowerFs() = default;
    virtual int lstat(const std::string& path, lx::stat& st) = 0;
    virtual int readlink(const std::string& path, std::string& target) = 0;
    virtual int readdir(const std::string& path, std::vector<DirEntryInfo>& out) = 0;
    virtual std::unique_ptr<FileSource> open(const std::string& path, int& err) = 0;
};

// Result of resolving a path.
struct Lookup {
    std::shared_ptr<TmpNode> upper;   // set when the name lives in the upper layer
    std::string canon;                // canonical path (symlinks resolved)
    lx::stat st {};
    bool lower = false;               // the name is a lower node (upper == nullptr)
    bool exists() const { return upper || lower; }
};

class Overlay {
public:
    explicit Overlay(LowerFs* lower = nullptr);
    void set_lower(LowerFs* lower) { lower_ = lower; }

    // Boot-time upper directories (mode includes the sticky bit if wanted).
    void mkdir_boot(const std::string& path, uint32_t mode, bool opaque);

    // ---- lookups ----
    int lookup(const std::string& path, bool follow, Lookup& out);
    int stat(const std::string& path, bool follow, lx::stat& st);
    int readlink(const std::string& path, std::string& target);
    int readdir(const std::string& path, std::vector<DirEntryInfo>& out);
    // Byte source of a regular file (upper: the live data; lower: the lower source).
    std::unique_ptr<FileSource> open_source(const std::string& path, int& err);

    // ---- mutations ----
    // O_CREAT: creates a regular file unless the name exists (then EEXIST with
    // excl, else the existing node/lower file is returned in `out`).
    int create(const std::string& path, uint32_t mode, bool excl, Lookup& out);
    // The upper node of a regular file for writing; copies a lower file up.
    int for_write(const std::string& path, bool follow, std::shared_ptr<TmpNode>& node);
    int mkdir(const std::string& path, uint32_t mode);
    int rmdir(const std::string& path);
    int unlink(const std::string& path);
    int rename(const std::string& from, const std::string& to, unsigned flags);  // lx::rename_*
    int link(const std::string& from, const std::string& to);
    int symlink(const std::string& target, const std::string& path);
    // Sockets and FIFOs (S_IFSOCK / S_IFIFO in mode). Returns the node.
    int mknod(const std::string& path, uint32_t mode, std::shared_ptr<TmpNode>& node);
    int chmod(const std::string& path, bool follow, uint32_t perm);
    int chown(const std::string& path, bool follow, uint32_t uid, uint32_t gid);
    int utimens(const std::string& path, bool follow, int64_t atime, int64_t mtime);
    int truncate(const std::string& path, uint64_t len);

    static uint64_t new_ino();
    static void fill_stat(const TmpNode& n, lx::stat& st);
    static int64_t now_sec();

private:
    LowerFs* lower_;
    std::mutex mu_;                         // the upper tree
    std::shared_ptr<TmpNode> root_;

    struct Walk {                           // resolve() output
        std::shared_ptr<TmpNode> parent;    // upper directory holding the name (may be null)
        std::shared_ptr<TmpNode> node;      // upper node (may be null)
        std::string canon;                  // canonical path
        std::string name;                   // last component
        bool lower_dir_ok = false;          // the lower tree is visible at the parent
        bool lower = false;                 // the name is a lower node
        lx::stat st {};
    };
    // mu_ held. depth: symlink expansion count.
    int resolve_locked(const std::string& path, bool follow, Walk& w, int depth);
    // mu_ held: canonical parent directory of `path` as an upper Dir node
    // (created from the lower directory's metadata when needed).
    int parent_dir_locked(const std::string& path, std::shared_ptr<TmpNode>& dir, std::string& canon_parent,
                          std::string& name, bool& lower_dir_ok);
    int copy_up_locked(const std::string& canon, const lx::stat& st, std::shared_ptr<TmpNode> parent,
                       const std::string& name, std::shared_ptr<TmpNode>& node);
    // mu_ held: the upper Dir node for the canonical lower directory `canon`
    // (every component created from the lower directory's metadata).
    int upper_chain_locked(const std::string& canon, std::shared_ptr<TmpNode>& dir);
    bool lower_has_locked(const std::string& canon);   // a lower node exists at this canonical path
    void remove_locked(const std::shared_ptr<TmpNode>& parent, const std::string& name, const std::string& canon,
                       bool lower_dir_ok);
    int merged_empty_locked(const Walk& w, bool& empty);
    // mu_ held: an upper node whose metadata can change (copies lower files
    // up, materializes lower directories).
    int upper_for_meta_locked(Walk& w, std::shared_ptr<TmpNode>& node);
    std::shared_ptr<TmpNode> make_node(TmpNode::Kind kind, uint32_t mode);
};

// A regular file in the upper layer: read/write/pread/pwrite/lseek/ftruncate.
class TmpFile final : public OpenFile {
public:
    TmpFile(std::shared_ptr<TmpNode> node, std::string guest_path, int oflags);
    ~TmpFile() override;
    int64_t read(void* buf, size_t len) override;
    int64_t write(const void* buf, size_t len) override;
    int64_t pread(void* buf, size_t len, uint64_t off) override;
    int64_t pwrite(const void* buf, size_t len, uint64_t off) override;
    int64_t lseek(int64_t off, int whence) override;
    int fstat(lx::stat& st) override;
    int ftruncate(uint64_t len) override;
    std::shared_ptr<FileSource> source() override;
    std::shared_ptr<TmpNode> node() { return node_; }

private:
    std::shared_ptr<TmpNode> node_;
    uint64_t pos_ = 0;
};

// Live view of an upper file's bytes (mmap population, the loader).
class TmpFileSource final : public FileSource {
public:
    TmpFileSource(std::shared_ptr<TmpNode> node, std::string path) : node_(std::move(node)), path_(std::move(path)) {}
    uint64_t size() const override;
    int64_t pread(void* buf, size_t len, uint64_t off) override;
    const std::string& path() const override { return path_; }
    uint64_t inode() const override { return node_->ino; }
    std::shared_ptr<TmpNode> node() { return node_; }

private:
    std::shared_ptr<TmpNode> node_;
    std::string path_;
};

}  // namespace rlk
