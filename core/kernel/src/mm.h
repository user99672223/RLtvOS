// mm.h — a guest process's address space. Guest address == host address;
// all guest processes share the one host address space (kernel-design §2),
// so a process only *books* what it owns and asks the host for memory
// lazily. Guest pages are 4 KB, host pages 16 KB (Apple silicon) or 4 KB
// (Linux host for tests): per-4K protections are kept in a shadow table and
// the host page gets the union.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace rlk {

struct Vma {
    uint64_t start = 0, end = 0;   // 4 KB aligned, [start, end)
    int flags = 0;                 // lx::map_* as requested (bookkeeping)
    bool alias = false;            // host pages are a shared alias of a file store (not private memory)
    std::string name;              // "[stack]", "[heap]", a guest path, or ""
    uint64_t file_off = 0;
    uint64_t ino = 0;
    std::vector<uint8_t> prot4k;   // lx::prot_* per 4 KB page

    uint64_t size() const { return end - start; }
    int prot_at(uint64_t addr) const { return prot4k[(addr - start) / 4096]; }
};

class AddressSpace {
public:
    AddressSpace();
    ~AddressSpace();
    AddressSpace(const AddressSpace&) = delete;
    AddressSpace& operator=(const AddressSpace&) = delete;

    // Map `len` bytes (rounded up to 4 KB) of zeroed memory.
    //   hint == 0            : the host picks the address.
    //   hint != 0, !fixed    : try the hint, else anywhere.
    //   fixed                : MAP_FIXED: replaces this process's own mappings
    //                          in the range; memory owned by anyone else
    //                          (another guest process, the app, FEX) is a
    //                          conflict → -ENOMEM.
    //   noreplace            : with fixed: -EEXIST if any byte is mapped.
    // Returns the guest address or -errno (Linux numbering).
    int64_t map(uint64_t hint, uint64_t len, int prot, bool fixed, bool noreplace, std::string name,
                uint64_t file_off = 0, uint64_t ino = 0);
    int64_t unmap(uint64_t addr, uint64_t len);
    int64_t protect(uint64_t addr, uint64_t len, int prot);
    // Write into the mapping regardless of its guest protection (loader,
    // file-backed mmap population). Returns false if the range is not mapped.
    bool copy_in(uint64_t addr, const void* src, size_t len);
    bool zero(uint64_t addr, size_t len);

    // MAP_SHARED of a writable file: replaces the host pages of the own VMA at
    // [addr, addr+hlen) (host-page aligned, exclusively ours) with an alias of
    // `store` at `off`, so writes are the file's writes. False = not possible
    // here (the caller falls back to a private copy with write-back).
    bool alias_shared(uint64_t addr, uint64_t hlen, class SharedStore* store, uint64_t off, int prot);

    bool is_mapped(uint64_t addr, uint64_t len) const;  // fully covered by own VMAs
    // Copy of the VMA containing addr (empty optional-like: end == 0 when none).
    Vma find(uint64_t addr) const;
    std::vector<Vma> vmas() const;
    std::string maps_text() const;  // /proc/<pid>/maps
    uint64_t mapped_bytes() const;
    size_t vma_count() const;

    // ---- fork support: copy-on-write snapshots --------------------------------
    // A forked child runs in its parent's address space while the parent is
    // suspended (vfork-style). push_snapshot() write-protects every writable
    // host page and copies the VMA table + brk; until pop_snapshot(), the
    // first write to a tracked page (a guest store → host fault →
    // cow_fault(); kernel writes → touched internally) saves the page's old
    // contents. pop_snapshot() restores the saved pages, the VMA table and
    // brk, and unmaps what was mapped since, so the parent resumes with its
    // memory exactly as it left it. Snapshots nest (a child forking again).
    void push_snapshot();
    void pop_snapshot();
    bool cow_fault(uint64_t addr);   // true: the write is now allowed, retry it
    bool snapshot_active() const;

    // Program break, managed by the process (bookkeeping lives here so a
    // vfork child sharing the space sees the same values).
    uint64_t brk_start = 0, brk_cur = 0, brk_end = 0;

    // Called (process-wide) with every guest range whose contents or
    // protection changed, so translated code for it can be dropped. FEX's
    // shared lookup cache keeps guest-address → host-code mappings across
    // threads; without this a remapped page executes stale translations.
    using InvalidateFn = void (*)(uint64_t start, uint64_t len);
    static void set_invalidate_hook(InvalidateFn fn);

    static uint64_t host_page();
    // Lowest address the host can map at all: 4 GB on tvOS/macOS arm64 (XNU
    // refuses to exec a 64-bit arm64 binary without a 4 GB hard page zero,
    // bsd/kern/mach_loader.c), 64 KB on Linux (vm.mmap_min_addr). Fixed
    // guest mappings below it fail with ENOMEM; hints below it are ignored.
    static uint64_t min_addr();
    static uint64_t host_down(uint64_t a) { return a & ~(host_page() - 1); }
    static uint64_t host_up(uint64_t a) { return (a + host_page() - 1) & ~(host_page() - 1); }
    static int to_host_prot(int guest_prot);

private:
    mutable std::mutex mu_;
    std::map<uint64_t, Vma> vmas_;  // by start

    // --- helpers, mu_ held ---
    bool covered_by_own(uint64_t hp) const;  // any own VMA overlaps host page hp
    bool alias_in_page_locked(uint64_t hp) const;  // an alias VMA overlaps host page hp
    int union_prot(uint64_t hp) const;       // guest prot union over host page hp
    void apply_host_prot(uint64_t start, uint64_t end);
    void erase_range_locked(uint64_t start, uint64_t end);
    void insert_locked(Vma v);
    int64_t map_fixed_locked(uint64_t start, uint64_t len, int prot, bool noreplace, std::string name, uint64_t file_off,
                             uint64_t ino);
    void release_uncovered_host_pages(uint64_t start, uint64_t end);
    bool make_writable(uint64_t start, uint64_t end);

    struct Snapshot {
        std::map<uint64_t, Vma> vmas;
        uint64_t brk_start, brk_cur, brk_end;
        std::map<uint64_t, std::vector<uint8_t>> pages;  // host page → contents before the first write
        std::vector<uint64_t> tracked;                     // sorted host pages write-protected for tracking
        bool is_tracked(uint64_t hp) const;
    };
    std::vector<Snapshot> snaps_;
    bool cow_tracked_locked(uint64_t hp) const;      // tracked and not yet saved in the top snapshot
    void cow_touch_locked(uint64_t start, uint64_t end);  // save before a kernel write
    std::vector<uint64_t> covered_host_pages_locked(const std::map<uint64_t, Vma>& vmas) const;
};

}  // namespace rlk
