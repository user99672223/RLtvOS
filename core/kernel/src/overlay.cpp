// overlay.cpp — see overlay.h.
#include "overlay.h"

#include <sys/mman.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/vm_map.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <ctime>

#include "locks.h"
#include "log.h"
#include "mm.h"
#include "pipe.h"

namespace rlk {

// ---- SharedStore / TmpData ---------------------------------------------------------

std::unique_ptr<SharedStore> SharedStore::Create(size_t capacity) {
    const size_t hp = (size_t)AddressSpace::host_page();
    capacity = (capacity + hp - 1) / hp * hp;
    if (capacity == 0) capacity = hp;
    std::unique_ptr<SharedStore> s(new SharedStore());
    s->cap_ = capacity;
#ifdef __APPLE__
    void* p = ::mmap(nullptr, capacity, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    s->base_ = static_cast<uint8_t*>(p);
#else
    int fd = (int)::memfd_create("rltvos-shared-file", MFD_CLOEXEC);
    if (fd < 0) return nullptr;
    if (::ftruncate(fd, (off_t)capacity) != 0) {
        ::close(fd);
        return nullptr;
    }
    void* p = ::mmap(nullptr, capacity, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        ::close(fd);
        return nullptr;
    }
    s->fd_ = fd;
    s->base_ = static_cast<uint8_t*>(p);
#endif
    return s;
}

SharedStore::~SharedStore() {
    if (base_) ::munmap(base_, cap_);
    if (fd_ >= 0) ::close(fd_);
}

bool SharedStore::alias(uint64_t addr, size_t len, size_t off, int hprot) {
    const size_t hp = (size_t)AddressSpace::host_page();
    if ((addr | len | off) & (hp - 1)) return false;
    if (off + len > cap_) return false;
#ifdef __APPLE__
    vm_address_t target = (vm_address_t)addr;
    vm_prot_t cur = 0, max = 0;
    kern_return_t kr = vm_remap(mach_task_self(), &target, (vm_size_t)len, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                                mach_task_self(), (vm_address_t)(base_ + off), FALSE, &cur, &max, VM_INHERIT_SHARE);
    if (kr != KERN_SUCCESS || target != (vm_address_t)addr) {
        Log("overlay: vm_remap(0x%llx+%zu ← store+%zu) failed kr=%d", (unsigned long long)addr, len, off, (int)kr);
        return false;
    }
    if (::mprotect(reinterpret_cast<void*>(addr), len, hprot) != 0) return false;
    return true;
#else
    void* p = ::mmap(reinterpret_cast<void*>(addr), len, hprot, MAP_SHARED | MAP_FIXED, fd_, (off_t)off);
    if (p == MAP_FAILED) {
        Log("overlay: shared alias mmap(0x%llx+%zu) failed errno=%d", (unsigned long long)addr, len, errno);
        return false;
    }
    return true;
#endif
}

bool TmpData::resize(size_t n) {
    if (!store) {
        bytes.resize(n);
        return true;
    }
    if (n > store->capacity()) return false;
    if (n > store_size) memset(store->base() + store_size, 0, n - store_size);
    if (n < store_size) memset(store->base() + n, 0, store_size - n);  // a later regrow reads zeros
    store_size = n;
    return true;
}

bool TmpData::ensure_store(size_t min_capacity) {
    if (store) return store->capacity() >= min_capacity;
    // Room to grow: files are mapped at their final size mostly, but leave
    // slack (the store cannot move once aliased).
    size_t cap = std::max<size_t>({min_capacity, bytes.size() * 2, (size_t)1 << 20});
    auto st = SharedStore::Create(cap);
    if (!st) return false;
    memcpy(st->base(), bytes.data(), bytes.size());
    store_size = bytes.size();
    store = std::move(st);
    bytes.clear();
    bytes.shrink_to_fit();
    return true;
}

namespace {

std::atomic<uint64_t> g_ino {0x200000};
constexpr uint64_t kUpperDev = 0x14;   // tmpfs-like device number for upper nodes
constexpr int kMaxSymlinks = 40;

std::vector<std::string> split(const std::string& path) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && path[i] == '/') i++;
        size_t j = i;
        while (j < path.size() && path[j] != '/') j++;
        if (j > i) out.emplace_back(path.substr(i, j - i));
        i = j;
    }
    return out;
}

std::string dirname_of(const std::string& p) {
    size_t s = p.rfind('/');
    if (s == std::string::npos || s == 0) return "/";
    return p.substr(0, s);
}

std::string basename_of(const std::string& p) {
    size_t s = p.rfind('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

std::string join_rest(const std::vector<std::string>& comps, size_t from) {
    std::string s;
    for (size_t k = from; k < comps.size(); k++) {
        s += "/";
        s += comps[k];
    }
    return s;
}

std::string child_path(const std::string& dir, const std::string& name) {
    return dir == "/" ? "/" + name : dir + "/" + name;
}

uint8_t dtype_of(const TmpNode& n) {
    switch (n.kind) {
        case TmpNode::Dir: return lx::dt_dir;
        case TmpNode::Reg: return lx::dt_reg;
        case TmpNode::Lnk: return lx::dt_lnk;
        case TmpNode::Sock: return lx::dt_sock;
        case TmpNode::Fifo: return lx::dt_fifo;
        default: return lx::dt_unknown;
    }
}

bool is_dir_mode(uint32_t mode) {
    return (mode & lx::s_ifmt) == lx::s_ifdir;
}

}  // namespace

// ---- TmpNode ---------------------------------------------------------------------

uint64_t TmpNode::size() const {
    switch (kind) {
        case Reg: {
            std::lock_guard<std::mutex> lk(data->mu);
            return data->size();
        }
        case Lnk: return target.size();
        case Dir: return 4096;
        default: return 0;
    }
}

// ---- Overlay: helpers -----------------------------------------------------------

Overlay::Overlay(LowerFs* lower) : lower_(lower) {
    root_ = make_node(TmpNode::Dir, lx::s_ifdir | 0755);
    root_->uid = root_->gid = 0;
}

uint64_t Overlay::new_ino() {
    return g_ino.fetch_add(1);
}

int64_t Overlay::now_sec() {
    return (int64_t)::time(nullptr);
}

void Overlay::fill_stat(const TmpNode& n, lx::stat& st) {
    FillStat(st, n.mode, n.size(), n.mtime, n.ino, 0, n.uid, n.gid);
    st.st_dev = kUpperDev;
    st.st_nlink = n.nlink;
    st.st_atim.tv_sec = n.atime;
    st.st_ctim.tv_sec = n.ctime;
}

std::shared_ptr<TmpNode> Overlay::make_node(TmpNode::Kind kind, uint32_t mode) {
    auto n = std::make_shared<TmpNode>();
    n->kind = kind;
    n->mode = mode;
    n->ino = new_ino();
    n->atime = n->mtime = n->ctime = now_sec();
    if (kind == TmpNode::Dir) n->nlink = 2;
    if (kind == TmpNode::Reg) n->data = std::make_shared<TmpData>();
    if (kind == TmpNode::Fifo) n->fifo = std::make_shared<Pipe>();
    return n;
}

void Overlay::mkdir_boot(const std::string& path, uint32_t mode, bool opaque) {
    std::lock_guard<std::mutex> lk(mu_);
    auto d = root_;
    std::string cur;
    const auto comps = split(path);
    for (size_t i = 0; i < comps.size(); i++) {
        cur += "/" + comps[i];
        auto it = d->children.find(comps[i]);
        if (it != d->children.end() && it->second->kind == TmpNode::Dir) {
            d = it->second;
        } else {
            const bool last = i + 1 == comps.size();
            uint32_t m = lx::s_ifdir | 0755;
            if (!last && lower_) {  // intermediate: mirror the lower directory's mode when it exists
                lx::stat st;
                if (lower_->lstat(cur, st) == 0 && is_dir_mode(st.st_mode)) m = st.st_mode;
            }
            auto n = make_node(TmpNode::Dir, m);
            n->uid = n->gid = 0;
            d->children[comps[i]] = n;
            d = n;
        }
    }
    d->mode = lx::s_ifdir | (mode & 07777);
    d->opaque = opaque;
}

int Overlay::resolve_locked(const std::string& path, bool follow, Walk& w, int depth) {
    if (depth > kMaxSymlinks) return lx::eloop;
    w = Walk {};
    const auto comps = split(path);
    if (comps.empty()) {
        w.node = root_;
        w.canon = "/";
        fill_stat(*root_, w.st);
        w.lower_dir_ok = true;
        return 0;
    }
    std::shared_ptr<TmpNode> upper = root_;   // upper directory at `cur` (null once we left the upper tree)
    bool lower_ok = !root_->opaque;           // the lower tree is visible at `cur`
    std::string cur;
    for (size_t i = 0; i < comps.size(); i++) {
        const std::string& c = comps[i];
        const bool last = i + 1 == comps.size();
        std::shared_ptr<TmpNode> child;
        if (upper) {
            auto it = upper->children.find(c);
            if (it != upper->children.end()) child = it->second;
        }
        if (child && child->kind == TmpNode::Whiteout) {
            if (last) {
                w.parent = upper;
                w.canon = cur + "/" + c;
                w.name = c;
                w.lower_dir_ok = false;
            }
            return lx::enoent;
        }
        if (child) {
            if (child->kind == TmpNode::Lnk && (!last || follow)) {
                const std::string base = child->target.empty() || child->target[0] != '/' ? cur + "/" + child->target
                                                                                          : child->target;
                return resolve_locked(NormalizePath(base + join_rest(comps, i + 1)), follow, w, depth + 1);
            }
            cur += "/" + c;
            if (last) {
                w.parent = upper;
                w.node = child;
                w.canon = cur;
                w.name = c;
                w.lower_dir_ok = lower_ok;
                fill_stat(*child, w.st);
                return 0;
            }
            if (child->kind != TmpNode::Dir) return lx::enotdir;
            upper = child;
            lower_ok = lower_ok && !child->opaque;
            continue;
        }
        // Not in the upper layer: the lower tree, if visible here.
        const std::string lp = cur + "/" + c;
        if (!lower_ok || !lower_) {
            if (last) {
                w.parent = upper;
                w.canon = lp;
                w.name = c;
                w.lower_dir_ok = false;
            }
            return lx::enoent;
        }
        lx::stat st {};
        int e = lower_->lstat(lp, st);
        if (e) {
            if (last) {
                w.parent = upper;
                w.canon = lp;
                w.name = c;
                w.lower_dir_ok = true;
            }
            return e;
        }
        if ((st.st_mode & lx::s_ifmt) == lx::s_iflnk && (!last || follow)) {
            std::string target;
            e = lower_->readlink(lp, target);
            if (e) return e;
            const std::string base = target.empty() || target[0] != '/' ? cur + "/" + target : target;
            return resolve_locked(NormalizePath(base + join_rest(comps, i + 1)), follow, w, depth + 1);
        }
        cur = lp;
        if (last) {
            w.parent = upper;
            w.canon = cur;
            w.name = c;
            w.lower_dir_ok = true;
            w.lower = true;
            w.st = st;
            return 0;
        }
        if (!is_dir_mode(st.st_mode)) return lx::enotdir;
        upper = nullptr;   // nothing of the upper tree exists below a lower-only directory
    }
    return lx::enoent;  // unreachable
}

int Overlay::upper_chain_locked(const std::string& canon, std::shared_ptr<TmpNode>& dir) {
    auto d = root_;
    std::string cur;
    for (const auto& c : split(canon)) {
        cur += "/" + c;
        auto it = d->children.find(c);
        if (it != d->children.end()) {
            if (it->second->kind != TmpNode::Dir) return lx::enotdir;
            d = it->second;
            continue;
        }
        lx::stat st {};
        uint32_t mode = lx::s_ifdir | 0755;
        uint32_t uid = 1000, gid = 1000;
        int64_t mtime = now_sec();
        if (lower_ && lower_->lstat(cur, st) == 0) {
            if (!is_dir_mode(st.st_mode)) return lx::enotdir;
            mode = st.st_mode;
            uid = st.st_uid;
            gid = st.st_gid;
            mtime = st.st_mtim.tv_sec;
        }
        auto n = make_node(TmpNode::Dir, mode);
        n->uid = uid;
        n->gid = gid;
        n->mtime = n->ctime = mtime;
        d->children[c] = n;
        d = n;
    }
    dir = d;
    return 0;
}

int Overlay::parent_dir_locked(const std::string& path, std::shared_ptr<TmpNode>& dir, std::string& canon_parent,
                               std::string& name, bool& lower_dir_ok) {
    name = basename_of(path);
    if (name.empty() || name == "." || name == "..") return lx::einval;
    Walk w;
    int e = resolve_locked(dirname_of(path), true, w, 0);
    if (e) return e;
    canon_parent = w.canon;
    if (w.node) {
        if (w.node->kind != TmpNode::Dir) return lx::enotdir;
        dir = w.node;
        lower_dir_ok = w.lower_dir_ok && !w.node->opaque;
        return 0;
    }
    if (!is_dir_mode(w.st.st_mode)) return lx::enotdir;
    lower_dir_ok = true;
    return upper_chain_locked(w.canon, dir);
}

bool Overlay::lower_has_locked(const std::string& canon) {
    if (!lower_) return false;
    lx::stat st {};
    return lower_->lstat(canon, st) == 0;
}

void Overlay::remove_locked(const std::shared_ptr<TmpNode>& parent, const std::string& name, const std::string& canon,
                            bool lower_dir_ok) {
    parent->children.erase(name);
    if (lower_dir_ok && lower_has_locked(canon)) {
        auto wh = std::make_shared<TmpNode>();
        wh->kind = TmpNode::Whiteout;
        parent->children[name] = wh;
    }
    parent->mtime = parent->ctime = now_sec();
}

int Overlay::copy_up_locked(const std::string& canon, const lx::stat& st, std::shared_ptr<TmpNode> parent,
                            const std::string& name, std::shared_ptr<TmpNode>& node) {
    int err = 0;
    auto src = lower_ ? lower_->open(canon, err) : nullptr;
    if (!src) return err ? err : lx::enoent;
    auto n = make_node(TmpNode::Reg, st.st_mode);
    n->uid = st.st_uid;
    n->gid = st.st_gid;
    n->mtime = st.st_mtim.tv_sec;
    const uint64_t size = src->size();
    n->data->resize(size);
    uint64_t done = 0;
    while (done < size) {
        int64_t got = src->pread(n->data->data() + done, (size_t)std::min<uint64_t>(size - done, 1u << 20), done);
        if (got <= 0) return got < 0 ? (int)-got : lx::eio;
        done += (uint64_t)got;
    }
    parent->children[name] = n;
    node = n;
    return 0;
}

int Overlay::upper_for_meta_locked(Walk& w, std::shared_ptr<TmpNode>& node) {
    if (w.node) {
        node = w.node;
        return 0;
    }
    const uint32_t type = w.st.st_mode & lx::s_ifmt;
    if (type == lx::s_ifdir) return upper_chain_locked(w.canon, node);
    if (type != lx::s_ifreg) return lx::eperm;  // lower symlinks and device nodes stay as they are
    std::shared_ptr<TmpNode> dir;
    std::string cp, name;
    bool lok;
    int e = parent_dir_locked(w.canon, dir, cp, name, lok);
    if (e) return e;
    return copy_up_locked(w.canon, w.st, dir, name, node);
}

int Overlay::merged_empty_locked(const Walk& w, bool& empty) {
    empty = true;
    bool lower_visible = w.lower;
    if (w.node) {
        for (auto& [name, c] : w.node->children) {
            if (c->kind != TmpNode::Whiteout) {
                empty = false;
                return 0;
            }
        }
        lower_visible = w.lower_dir_ok && !w.node->opaque;
    }
    if (!lower_visible || !lower_) return 0;
    std::vector<DirEntryInfo> ents;
    int e = lower_->readdir(w.canon, ents);
    if (e) return e == lx::enoent ? 0 : e;
    for (auto& d : ents) {
        if (d.name == "." || d.name == "..") continue;
        if (w.node && w.node->children.count(d.name)) continue;  // whiteout (overrides are handled above)
        empty = false;
        return 0;
    }
    return 0;
}

// ---- Overlay: lookups ---------------------------------------------------------------

int Overlay::lookup(const std::string& path, bool follow, Lookup& out) {
    std::lock_guard<std::mutex> lk(mu_);
    Walk w;
    int e = resolve_locked(path, follow, w, 0);
    out = Lookup {};
    out.canon = w.canon;
    if (e) return e;
    out.upper = w.node;
    out.lower = w.lower;
    out.st = w.st;
    return 0;
}

int Overlay::stat(const std::string& path, bool follow, lx::stat& st) {
    Lookup l;
    int e = lookup(path, follow, l);
    if (e) return e;
    st = l.st;
    return 0;
}

int Overlay::readlink(const std::string& path, std::string& target) {
    std::lock_guard<std::mutex> lk(mu_);
    Walk w;
    int e = resolve_locked(path, false, w, 0);
    if (e) return e;
    if (w.node) {
        if (w.node->kind != TmpNode::Lnk) return lx::einval;
        target = w.node->target;
        return 0;
    }
    if ((w.st.st_mode & lx::s_ifmt) != lx::s_iflnk) return lx::einval;
    return lower_ ? lower_->readlink(w.canon, target) : lx::einval;
}

int Overlay::readdir(const std::string& path, std::vector<DirEntryInfo>& out) {
    std::lock_guard<std::mutex> lk(mu_);
    Walk w;
    int e = resolve_locked(path, true, w, 0);
    if (e) return e;
    if (!w.node) {
        if (!is_dir_mode(w.st.st_mode)) return lx::enotdir;
        return lower_ ? lower_->readdir(w.canon, out) : lx::enoent;
    }
    if (w.node->kind != TmpNode::Dir) return lx::enotdir;
    out.push_back({".", lx::dt_dir, w.node->ino});
    out.push_back({"..", lx::dt_dir, w.parent ? w.parent->ino : w.node->ino});
    for (auto& [name, c] : w.node->children) {
        if (c->kind == TmpNode::Whiteout) continue;
        out.push_back({name, dtype_of(*c), c->ino});
    }
    if (lower_ && w.lower_dir_ok && !w.node->opaque) {
        std::vector<DirEntryInfo> lower;
        if (lower_->readdir(w.canon, lower) == 0) {
            for (auto& d : lower) {
                if (d.name == "." || d.name == "..") continue;
                if (w.node->children.count(d.name)) continue;
                out.push_back(d);
            }
        }
    }
    return 0;
}

std::unique_ptr<FileSource> Overlay::open_source(const std::string& path, int& err) {
    std::lock_guard<std::mutex> lk(mu_);
    Walk w;
    err = resolve_locked(path, true, w, 0);
    if (err) return nullptr;
    if (w.node) {
        if (w.node->kind == TmpNode::Dir) {
            err = lx::eisdir;
            return nullptr;
        }
        if (w.node->kind != TmpNode::Reg) {
            err = lx::enxio;
            return nullptr;
        }
        return std::make_unique<TmpFileSource>(w.node, w.canon);
    }
    if (!lower_) {
        err = lx::enoent;
        return nullptr;
    }
    return lower_->open(w.canon, err);
}

// ---- Overlay: mutations ------------------------------------------------------------

int Overlay::create(const std::string& path, uint32_t mode, bool excl, Lookup& out) {
    std::lock_guard<std::mutex> lk(mu_);
    Walk w;
    int e = resolve_locked(path, true, w, 0);
    out = Lookup {};
    if (e == 0) {
        if (excl) return lx::eexist;
        out.upper = w.node;
        out.lower = w.lower;
        out.canon = w.canon;
        out.st = w.st;
        return 0;
    }
    if (e != lx::enoent) return e;
    std::shared_ptr<TmpNode> dir;
    std::string cp, name;
    bool lok;
    e = parent_dir_locked(path, dir, cp, name, lok);
    if (e) return e;
    auto it = dir->children.find(name);
    if (it != dir->children.end() && it->second->kind != TmpNode::Whiteout) return lx::eexist;  // dangling symlink
    auto n = make_node(TmpNode::Reg, lx::s_ifreg | (mode & 07777));
    dir->children[name] = n;
    dir->mtime = dir->ctime = now_sec();
    out.upper = n;
    out.canon = child_path(cp, name);
    fill_stat(*n, out.st);
    return 0;
}

int Overlay::for_write(const std::string& path, bool follow, std::shared_ptr<TmpNode>& node) {
    std::lock_guard<std::mutex> lk(mu_);
    Walk w;
    int e = resolve_locked(path, follow, w, 0);
    if (e) return e;
    if (w.node) {
        if (w.node->kind == TmpNode::Dir) return lx::eisdir;
        if (w.node->kind != TmpNode::Reg) return lx::enxio;
        node = w.node;
        return 0;
    }
    const uint32_t type = w.st.st_mode & lx::s_ifmt;
    if (type == lx::s_ifdir) return lx::eisdir;
    if (type != lx::s_ifreg) return lx::enxio;
    std::shared_ptr<TmpNode> dir;
    std::string cp, name;
    bool lok;
    e = parent_dir_locked(w.canon, dir, cp, name, lok);
    if (e) return e;
    return copy_up_locked(w.canon, w.st, dir, name, node);
}

int Overlay::mkdir(const std::string& path, uint32_t mode) {
    std::lock_guard<std::mutex> lk(mu_);
    Walk w;
    int e = resolve_locked(path, false, w, 0);
    if (e == 0) return lx::eexist;
    if (e != lx::enoent) return e;
    std::shared_ptr<TmpNode> dir;
    std::string cp, name;
    bool lok;
    e = parent_dir_locked(path, dir, cp, name, lok);
    if (e) return e;
    auto n = make_node(TmpNode::Dir, lx::s_ifdir | (mode & 07777));
    n->opaque = true;  // nothing of a hidden lower directory shows through a new one
    dir->children[name] = n;
    dir->nlink++;
    dir->mtime = dir->ctime = now_sec();
    return 0;
}

int Overlay::rmdir(const std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    Walk w;
    int e = resolve_locked(path, false, w, 0);
    if (e) return e;
    if (w.canon == "/") return lx::ebusy;
    if (w.node ? w.node->kind != TmpNode::Dir : !is_dir_mode(w.st.st_mode)) return lx::enotdir;
    bool empty;
    e = merged_empty_locked(w, empty);
    if (e) return e;
    if (!empty) return lx::enotempty;
    if (w.node) {
        remove_locked(w.parent, w.name, w.canon, w.lower_dir_ok);
        if (w.parent->nlink > 2) w.parent->nlink--;
        return 0;
    }
    std::shared_ptr<TmpNode> dir;
    std::string cp, name;
    bool lok;
    e = parent_dir_locked(w.canon, dir, cp, name, lok);
    if (e) return e;
    remove_locked(dir, name, w.canon, true);
    return 0;
}

int Overlay::unlink(const std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    Walk w;
    int e = resolve_locked(path, false, w, 0);
    if (e) return e;
    if (w.node ? w.node->kind == TmpNode::Dir : is_dir_mode(w.st.st_mode)) return lx::eisdir;
    if (w.node) {
        remove_locked(w.parent, w.name, w.canon, w.lower_dir_ok);
        if (w.node->nlink > 0) w.node->nlink--;
        w.node->ctime = now_sec();
        return 0;
    }
    std::shared_ptr<TmpNode> dir;
    std::string cp, name;
    bool lok;
    e = parent_dir_locked(w.canon, dir, cp, name, lok);
    if (e) return e;
    remove_locked(dir, name, w.canon, true);
    return 0;
}

int Overlay::rename(const std::string& from, const std::string& to, unsigned flags) {
    if (flags & ~(lx::rename_noreplace | lx::rename_exchange)) return lx::einval;
    std::lock_guard<std::mutex> lk(mu_);
    Walk wf, wt;
    int e = resolve_locked(from, false, wf, 0);
    if (e) return e;
    int et = resolve_locked(to, false, wt, 0);
    if (et && et != lx::enoent) return et;
    if (wf.canon == "/" || wt.canon == "/") return lx::ebusy;
    if (et == 0 && wf.canon == wt.canon) return 0;
    if ((flags & lx::rename_noreplace) && et == 0) return lx::eexist;
    // `to` inside `from` (directories)
    if (wt.canon.rfind(wf.canon + "/", 0) == 0) return lx::einval;
    if (flags & lx::rename_exchange) {
        if (et) return et;
        if (!wf.node || !wt.node) return lx::exdev;  // exchanging with a lower name is not supported
        std::swap(wf.parent->children[wf.name], wt.parent->children[wt.name]);
        return 0;
    }
    // Source node to move (a lower regular file is copied up first; lower
    // directories and merged directories cannot move).
    std::shared_ptr<TmpNode> src = wf.node;
    const bool src_is_dir = src ? src->kind == TmpNode::Dir : is_dir_mode(wf.st.st_mode);
    if (src) {
        if (src->kind == TmpNode::Dir && !src->opaque && wf.lower_dir_ok && lower_has_locked(wf.canon)) return lx::exdev;
    } else {
        const uint32_t type = wf.st.st_mode & lx::s_ifmt;
        if (type == lx::s_ifreg) {
            std::shared_ptr<TmpNode> dir;
            std::string cp, name;
            bool lok;
            e = parent_dir_locked(wf.canon, dir, cp, name, lok);
            if (e) return e;
            e = copy_up_locked(wf.canon, wf.st, dir, name, src);
            if (e) return e;
            wf.parent = dir;
            wf.name = name;
            wf.lower_dir_ok = true;
        } else if (type == lx::s_iflnk) {
            std::string target;
            e = lower_->readlink(wf.canon, target);
            if (e) return e;
            std::shared_ptr<TmpNode> dir;
            std::string cp, name;
            bool lok;
            e = parent_dir_locked(wf.canon, dir, cp, name, lok);
            if (e) return e;
            src = make_node(TmpNode::Lnk, lx::s_iflnk | 0777);
            src->target = target;
            dir->children[name] = src;
            wf.parent = dir;
            wf.name = name;
            wf.lower_dir_ok = true;
        } else {
            return lx::exdev;
        }
    }
    // Destination checks.
    if (et == 0) {
        const bool dst_is_dir = wt.node ? wt.node->kind == TmpNode::Dir : is_dir_mode(wt.st.st_mode);
        if (src_is_dir && !dst_is_dir) return lx::enotdir;
        if (!src_is_dir && dst_is_dir) return lx::eisdir;
        if (dst_is_dir) {
            bool empty;
            e = merged_empty_locked(wt, empty);
            if (e) return e;
            if (!empty) return lx::enotempty;
        }
    }
    std::shared_ptr<TmpNode> ddir;
    std::string dcp, dname;
    bool dlok;
    e = parent_dir_locked(to, ddir, dcp, dname, dlok);
    if (e) return e;
    // Detach from the old parent (leaving a whiteout when the lower tree has the name).
    remove_locked(wf.parent, wf.name, wf.canon, wf.lower_dir_ok);
    if (src_is_dir && wf.parent->nlink > 2) wf.parent->nlink--;
    // Attach under the new name (an existing upper node or whiteout is replaced).
    auto old = ddir->children.find(dname);
    const bool replaced_dir = old != ddir->children.end() && old->second->kind == TmpNode::Dir;
    ddir->children[dname] = src;
    if (src_is_dir && !replaced_dir) ddir->nlink++;
    if (src_is_dir && et == 0 && !wt.node && lower_has_locked(wt.canon)) src->opaque = true;  // hides the replaced lower dir
    ddir->mtime = ddir->ctime = now_sec();
    src->ctime = now_sec();
    return 0;
}

int Overlay::link(const std::string& from, const std::string& to) {
    std::lock_guard<std::mutex> lk(mu_);
    Walk wf, wt;
    int e = resolve_locked(from, false, wf, 0);
    if (e) return e;
    int et = resolve_locked(to, false, wt, 0);
    if (et == 0) return lx::eexist;
    if (et != lx::enoent) return et;
    std::shared_ptr<TmpNode> node = wf.node;
    if (node) {
        if (node->kind == TmpNode::Dir) return lx::eperm;
    } else {
        if (is_dir_mode(wf.st.st_mode)) return lx::eperm;
        if ((wf.st.st_mode & lx::s_ifmt) != lx::s_ifreg) return lx::exdev;
        std::shared_ptr<TmpNode> dir;
        std::string cp, name;
        bool lok;
        e = parent_dir_locked(wf.canon, dir, cp, name, lok);
        if (e) return e;
        e = copy_up_locked(wf.canon, wf.st, dir, name, node);
        if (e) return e;
    }
    std::shared_ptr<TmpNode> ddir;
    std::string dcp, dname;
    bool dlok;
    e = parent_dir_locked(to, ddir, dcp, dname, dlok);
    if (e) return e;
    ddir->children[dname] = node;
    node->nlink++;
    node->ctime = ddir->mtime = ddir->ctime = now_sec();
    return 0;
}

int Overlay::symlink(const std::string& target, const std::string& path) {
    if (target.empty()) return lx::enoent;
    std::lock_guard<std::mutex> lk(mu_);
    Walk w;
    int e = resolve_locked(path, false, w, 0);
    if (e == 0) return lx::eexist;
    if (e != lx::enoent) return e;
    std::shared_ptr<TmpNode> dir;
    std::string cp, name;
    bool lok;
    e = parent_dir_locked(path, dir, cp, name, lok);
    if (e) return e;
    auto n = make_node(TmpNode::Lnk, lx::s_iflnk | 0777);
    n->target = target;
    dir->children[name] = n;
    dir->mtime = dir->ctime = now_sec();
    return 0;
}

int Overlay::mknod(const std::string& path, uint32_t mode, std::shared_ptr<TmpNode>& node) {
    const uint32_t type = mode & lx::s_ifmt;
    TmpNode::Kind kind;
    if (type == lx::s_ifsock) {
        kind = TmpNode::Sock;
    } else if (type == lx::s_ififo) {
        kind = TmpNode::Fifo;
    } else if (type == lx::s_ifreg || type == 0) {
        kind = TmpNode::Reg;
        mode = lx::s_ifreg | (mode & 07777);
    } else {
        return lx::eperm;  // device nodes: not for an unprivileged guest
    }
    std::lock_guard<std::mutex> lk(mu_);
    Walk w;
    int e = resolve_locked(path, false, w, 0);
    if (e == 0) return lx::eexist;
    if (e != lx::enoent) return e;
    std::shared_ptr<TmpNode> dir;
    std::string cp, name;
    bool lok;
    e = parent_dir_locked(path, dir, cp, name, lok);
    if (e) return e;
    node = make_node(kind, mode);
    dir->children[name] = node;
    dir->mtime = dir->ctime = now_sec();
    return 0;
}

int Overlay::chmod(const std::string& path, bool follow, uint32_t perm) {
    std::lock_guard<std::mutex> lk(mu_);
    Walk w;
    int e = resolve_locked(path, follow, w, 0);
    if (e) return e;
    std::shared_ptr<TmpNode> n;
    e = upper_for_meta_locked(w, n);
    if (e) return e == lx::eperm ? 0 : e;  // lower symlinks/devices: accepted, unchanged
    n->mode = (n->mode & lx::s_ifmt) | (perm & 07777);
    n->ctime = now_sec();
    return 0;
}

int Overlay::chown(const std::string& path, bool follow, uint32_t uid, uint32_t gid) {
    std::lock_guard<std::mutex> lk(mu_);
    Walk w;
    int e = resolve_locked(path, follow, w, 0);
    if (e) return e;
    std::shared_ptr<TmpNode> n;
    e = upper_for_meta_locked(w, n);
    if (e) return e == lx::eperm ? 0 : e;
    if (uid != (uint32_t)-1) n->uid = uid;
    if (gid != (uint32_t)-1) n->gid = gid;
    n->ctime = now_sec();
    return 0;
}

int Overlay::utimens(const std::string& path, bool follow, int64_t atime, int64_t mtime) {
    std::lock_guard<std::mutex> lk(mu_);
    Walk w;
    int e = resolve_locked(path, follow, w, 0);
    if (e) return e;
    std::shared_ptr<TmpNode> n;
    e = upper_for_meta_locked(w, n);
    if (e) return e == lx::eperm ? 0 : e;
    if (atime != lx::utime_omit) n->atime = atime == lx::utime_now ? now_sec() : atime;
    if (mtime != lx::utime_omit) n->mtime = mtime == lx::utime_now ? now_sec() : mtime;
    n->ctime = now_sec();
    return 0;
}

int Overlay::truncate(const std::string& path, uint64_t len) {
    std::shared_ptr<TmpNode> n;
    int e = for_write(path, true, n);
    if (e) return e;
    std::lock_guard<std::mutex> lk(n->data->mu);
    if (!n->data->resize(len)) return lx::enospc;
    n->mtime = n->ctime = now_sec();
    return 0;
}

// ---- TmpFile -------------------------------------------------------------------------

TmpFile::TmpFile(std::shared_ptr<TmpNode> node, std::string guest_path, int oflags) : node_(std::move(node)) {
    path = std::move(guest_path);
    this->oflags = oflags;
}

TmpFile::~TmpFile() {
    if (has_locks) LockTable::get().release_ofd(this);
}

int64_t TmpFile::read(void* buf, size_t len) {
    if ((oflags & lx::o_accmode) == lx::o_wronly) return -lx::ebadf;
    std::lock_guard<std::mutex> lk(mu);
    int64_t n = pread(buf, len, pos_);
    if (n > 0) pos_ += (uint64_t)n;
    return n;
}

int64_t TmpFile::pread(void* buf, size_t len, uint64_t off) {
    if ((oflags & lx::o_accmode) == lx::o_wronly) return -lx::ebadf;
    std::lock_guard<std::mutex> lk(node_->data->mu);
    const TmpData& d = *node_->data;
    if (off >= d.size()) return 0;
    size_t n = std::min<size_t>(len, d.size() - off);
    memcpy(buf, d.data() + off, n);
    return (int64_t)n;
}

int64_t TmpFile::write(const void* buf, size_t len) {
    if ((oflags & lx::o_accmode) == lx::o_rdonly) return -lx::ebadf;
    std::lock_guard<std::mutex> lk(mu);
    if (oflags & lx::o_append) {
        std::lock_guard<std::mutex> dl(node_->data->mu);
        pos_ = node_->data->size();
    }
    int64_t n = pwrite(buf, len, pos_);
    if (n > 0) pos_ += (uint64_t)n;
    return n;
}

int64_t TmpFile::pwrite(const void* buf, size_t len, uint64_t off) {
    if ((oflags & lx::o_accmode) == lx::o_rdonly) return -lx::ebadf;
    if (len == 0) return 0;
    if (off + len > (1ull << 40)) return -lx::efbig;
    std::lock_guard<std::mutex> lk(node_->data->mu);
    TmpData& d = *node_->data;
    if (off + len > d.size() && !d.resize(off + len)) return -lx::enospc;
    memcpy(d.data() + off, buf, len);
    node_->mtime = node_->ctime = Overlay::now_sec();
    return (int64_t)len;
}

int64_t TmpFile::lseek(int64_t off, int whence) {
    std::lock_guard<std::mutex> lk(mu);
    int64_t base;
    switch (whence) {
        case lx::seek_set: base = 0; break;
        case lx::seek_cur: base = (int64_t)pos_; break;
        case lx::seek_end: base = (int64_t)node_->size(); break;
        default: return -lx::einval;
    }
    if (base + off < 0) return -lx::einval;
    pos_ = (uint64_t)(base + off);
    return (int64_t)pos_;
}

int TmpFile::fstat(lx::stat& st) {
    Overlay::fill_stat(*node_, st);
    return 0;
}

int TmpFile::ftruncate(uint64_t len) {
    if ((oflags & lx::o_accmode) == lx::o_rdonly) return -lx::einval;
    std::lock_guard<std::mutex> lk(node_->data->mu);
    if (!node_->data->resize(len)) return -lx::enospc;
    node_->mtime = node_->ctime = Overlay::now_sec();
    return 0;
}

std::shared_ptr<FileSource> TmpFile::source() {
    return std::make_shared<TmpFileSource>(node_, path);
}

uint64_t TmpFileSource::size() const {
    return node_->size();
}

int64_t TmpFileSource::pread(void* buf, size_t len, uint64_t off) {
    std::lock_guard<std::mutex> lk(node_->data->mu);
    const TmpData& d = *node_->data;
    if (off >= d.size()) return 0;
    size_t n = std::min<size_t>(len, d.size() - off);
    memcpy(buf, d.data() + off, n);
    return (int64_t)n;
}

}  // namespace rlk
