// mm.cpp — see mm.h.
#include "mm.h"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "linux_abi.h"
#include "log.h"

namespace rlk {

namespace {
constexpr uint64_t K4 = 4096;

// Try to get exactly [addr, addr+len) from the host without MAP_FIXED
// (so nothing already there is clobbered). Returns true on success.
bool host_try_exact(uint64_t addr, uint64_t len, int hprot) {
    void* p = ::mmap(reinterpret_cast<void*>(addr), len, hprot, host::kMapPrivate | host::kMapAnon, -1, 0);
    if (p == host::MapFailed()) return false;
    if (reinterpret_cast<uint64_t>(p) != addr) {
        ::munmap(p, len);
        return false;
    }
    return true;
}

AddressSpace::InvalidateFn g_invalidate = nullptr;

// Translated guest code may exist for any range that changes; tell FEX.
inline void invalidate(uint64_t start, uint64_t end) {
    if (g_invalidate && end > start) g_invalidate(start, end - start);
}
}  // namespace

void AddressSpace::set_invalidate_hook(InvalidateFn fn) {
    g_invalidate = fn;
}

uint64_t AddressSpace::host_page() {
    static const uint64_t ps = [] {
        long v = sysconf(_SC_PAGESIZE);
        return v > 0 ? (uint64_t)v : 16384u;
    }();
    return ps;
}

uint64_t AddressSpace::min_addr() {
#ifdef __APPLE__
    return 0x100000000ull;
#else
    return 0x10000ull;
#endif
}

int AddressSpace::to_host_prot(int p) {
    // Guest code is never executed by the host (FEX reads it), so no EXEC.
    int h = 0;
    if (p & (lx::prot_read | lx::prot_exec)) h |= host::kProtRead;
    if (p & lx::prot_write) h |= host::kProtRead | host::kProtWrite;
    return h;
}

AddressSpace::AddressSpace() = default;

AddressSpace::~AddressSpace() {
    std::lock_guard<std::mutex> lk(mu_);
    // Release everything we own, host page by host page (pages shared with
    // nobody else: all VMAs here are ours). Translations of the freed ranges
    // are dropped too: the next process may be loaded at the same addresses.
    while (!vmas_.empty()) {
        auto it = vmas_.begin();
        uint64_t s = it->second.start, e = it->second.end;
        vmas_.erase(it);
        release_uncovered_host_pages(s, e);
        invalidate(s, e);
    }
}

// ---- queries --------------------------------------------------------------------

bool AddressSpace::covered_by_own(uint64_t hp) const {
    uint64_t hpe = hp + host_page();
    auto it = vmas_.upper_bound(hp);
    if (it != vmas_.begin()) {
        auto prev = std::prev(it);
        if (prev->second.end > hp) return true;
    }
    return it != vmas_.end() && it->second.start < hpe;
}

int AddressSpace::union_prot(uint64_t hp) const {
    uint64_t hpe = hp + host_page();
    int u = 0;
    auto it = vmas_.upper_bound(hp);
    if (it != vmas_.begin()) --it;
    for (; it != vmas_.end() && it->second.start < hpe; ++it) {
        const Vma& v = it->second;
        uint64_t s = std::max(v.start, hp), e = std::min(v.end, hpe);
        for (uint64_t a = s; a < e; a += K4) u |= v.prot_at(a);
    }
    return u;
}

bool AddressSpace::is_mapped(uint64_t addr, uint64_t len) const {
    std::lock_guard<std::mutex> lk(mu_);
    uint64_t a = lx::PageDown(addr), end = lx::PageUp(addr + len);
    while (a < end) {
        auto it = vmas_.upper_bound(a);
        if (it == vmas_.begin()) return false;
        --it;
        if (it->second.end <= a) return false;
        a = it->second.end;
    }
    return true;
}

Vma AddressSpace::find(uint64_t addr) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = vmas_.upper_bound(addr);
    if (it == vmas_.begin()) return {};
    --it;
    if (it->second.end <= addr) return {};
    return it->second;
}

std::vector<Vma> AddressSpace::vmas() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Vma> out;
    out.reserve(vmas_.size());
    for (auto& [s, v] : vmas_) out.push_back(v);
    return out;
}

uint64_t AddressSpace::mapped_bytes() const {
    std::lock_guard<std::mutex> lk(mu_);
    uint64_t n = 0;
    for (auto& [s, v] : vmas_) n += v.size();
    return n;
}

size_t AddressSpace::vma_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return vmas_.size();
}

std::string AddressSpace::maps_text() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::string out;
    char line[512];
    for (auto& [s, v] : vmas_) {
        // split runs of equal protection like the kernel does
        uint64_t a = v.start;
        while (a < v.end) {
            int p = v.prot_at(a);
            uint64_t b = a + K4;
            while (b < v.end && v.prot_at(b) == p) b += K4;
            snprintf(line, sizeof line, "%012llx-%012llx %c%c%c%c %08llx 00:00 %llu %s%s\n", (unsigned long long)a,
                     (unsigned long long)b, (p & lx::prot_read) ? 'r' : '-', (p & lx::prot_write) ? 'w' : '-',
                     (p & lx::prot_exec) ? 'x' : '-', (v.flags & lx::map_shared) ? 's' : 'p',
                     (unsigned long long)(v.file_off + (a - v.start)), (unsigned long long)v.ino,
                     v.name.empty() ? "" : "                          ", v.name.c_str());
            out += line;
            a = b;
        }
    }
    return out;
}

// ---- host protection maintenance --------------------------------------------------

void AddressSpace::apply_host_prot(uint64_t start, uint64_t end) {
    uint64_t hp = host_down(start), he = host_up(end);
    uint64_t run_start = hp;
    int run_prot = -2;
    auto flush = [&](uint64_t upto) {
        if (run_prot >= 0 && upto > run_start) {
            ::mprotect(reinterpret_cast<void*>(run_start), upto - run_start, to_host_prot(run_prot));
        }
    };
    for (uint64_t p = hp; p < he; p += host_page()) {
        int u = covered_by_own(p) ? union_prot(p) : -1;
        if (u != run_prot) {
            flush(p);
            run_start = p;
            run_prot = u;
        }
    }
    flush(he);
}

void AddressSpace::release_uncovered_host_pages(uint64_t start, uint64_t end) {
    uint64_t hp = host_down(start), he = host_up(end);
    uint64_t run = 0;
    bool in_run = false;
    for (uint64_t p = hp; p <= he; p += host_page()) {
        bool free_it = p < he && !covered_by_own(p);
        if (free_it && !in_run) {
            run = p;
            in_run = true;
        } else if (!free_it && in_run) {
            ::munmap(reinterpret_cast<void*>(run), p - run);
            in_run = false;
        }
    }
    // host pages still partially owned keep the union protection
    apply_host_prot(start, end);
}

bool AddressSpace::make_writable(uint64_t start, uint64_t end) {
    return ::mprotect(reinterpret_cast<void*>(host_down(start)), host_up(end) - host_down(start),
                      host::kProtRead | host::kProtWrite) == 0;
}

// ---- bookkeeping ------------------------------------------------------------------

void AddressSpace::erase_range_locked(uint64_t start, uint64_t end) {
    auto it = vmas_.upper_bound(start);
    if (it != vmas_.begin()) --it;
    while (it != vmas_.end() && it->second.start < end) {
        Vma v = it->second;
        if (v.end <= start) {
            ++it;
            continue;
        }
        it = vmas_.erase(it);
        if (v.start < start) {  // keep the head
            Vma head = v;
            head.end = start;
            head.prot4k.resize((start - v.start) / K4);
            vmas_.emplace(head.start, std::move(head));
        }
        if (v.end > end) {  // keep the tail
            Vma tail;
            tail.start = end;
            tail.end = v.end;
            tail.flags = v.flags;
            tail.name = v.name;
            tail.ino = v.ino;
            tail.file_off = v.file_off + (end - v.start);
            tail.prot4k.assign(v.prot4k.begin() + (end - v.start) / K4, v.prot4k.end());
            it = vmas_.emplace(tail.start, std::move(tail)).first;
            ++it;
        }
    }
}

void AddressSpace::insert_locked(Vma v) {
    erase_range_locked(v.start, v.end);
    vmas_.emplace(v.start, std::move(v));
}

// ---- map / unmap / protect ------------------------------------------------------------

int64_t AddressSpace::map_fixed_locked(uint64_t start, uint64_t len, int prot, bool noreplace, std::string name,
                                       uint64_t file_off, uint64_t ino) {
    const uint64_t end = start + len;
    if (noreplace) {
        auto it = vmas_.upper_bound(start);
        if (it != vmas_.begin() && std::prev(it)->second.end > start) return -lx::eexist;
        if (it != vmas_.end() && it->second.start < end) return -lx::eexist;
    }
    // Host pages we already own (fully or shared with neighbouring own VMAs)
    // are zeroed in place; runs of host pages we do not own must be free on
    // the host (anything else there — another guest process, the app, FEX —
    // is a conflict). The union protection is applied at the end.
    const uint64_t hs = host_down(start), he = host_up(end);
    uint64_t run = 0;
    bool in_run = false;
    for (uint64_t hp = hs; hp <= he; hp += host_page()) {
        const bool own = hp < he && covered_by_own(hp);
        const bool want_new = hp < he && !own;
        if (want_new && !in_run) {
            run = hp;
            in_run = true;
        } else if (!want_new && in_run) {
            if (!host_try_exact(run, hp - run, host::kProtRead | host::kProtWrite)) {
                Log("mm: vma-conflict at 0x%llx+0x%llx (%s wanted 0x%llx+0x%llx)", (unsigned long long)run,
                    (unsigned long long)(hp - run), name.c_str(), (unsigned long long)start, (unsigned long long)len);
                return -lx::enomem;
            }
            in_run = false;
        }
        if (own) {
            const uint64_t hpe = hp + host_page();
            uint64_t zs = std::max(hp, start), ze = std::min(hpe, end);
            ::mprotect(reinterpret_cast<void*>(hp), host_page(), host::kProtRead | host::kProtWrite);
            memset(reinterpret_cast<void*>(zs), 0, ze - zs);
        }
    }
    Vma v;
    v.start = start;
    v.end = end;
    v.flags = 0;
    v.name = std::move(name);
    v.file_off = file_off;
    v.ino = ino;
    v.prot4k.assign(len / K4, (uint8_t)prot);
    insert_locked(std::move(v));
    apply_host_prot(start, end);
    invalidate(start, end);
    return (int64_t)start;
}

int64_t AddressSpace::map(uint64_t hint, uint64_t len, int prot, bool fixed, bool noreplace, std::string name,
                          uint64_t file_off, uint64_t ino) {
    if (len == 0) return -lx::einval;
    len = lx::PageUp(len);
    if (fixed) {
        if (hint & (K4 - 1)) return -lx::einval;
        if (hint < min_addr()) {
            Log("mm: fixed mapping 0x%llx+0x%llx (%s) is below the lowest mappable address 0x%llx", (unsigned long long)hint,
                (unsigned long long)len, name.c_str(), (unsigned long long)min_addr());
            return -lx::enomem;
        }
        std::lock_guard<std::mutex> lk(mu_);
        return map_fixed_locked(hint, len, prot, noreplace, std::move(name), file_off, ino);
    }
    std::lock_guard<std::mutex> lk(mu_);
    const uint64_t hlen = host_up(len);
    const int hprot = to_host_prot(prot);
    uint64_t addr = 0;
    if (hint && hint >= min_addr() && !(hint & (host_page() - 1)) && host_try_exact(hint, hlen, hprot)) {
        addr = hint;
    } else {
        void* p = ::mmap(nullptr, hlen, hprot, host::kMapPrivate | host::kMapAnon, -1, 0);
        if (p == host::MapFailed()) {
            Log("mm: host mmap(%llu KB) failed errno=%d", (unsigned long long)(hlen >> 10), errno);
            return -lx::enomem;
        }
        addr = reinterpret_cast<uint64_t>(p);
    }
    Vma v;
    v.start = addr;
    v.end = addr + len;
    v.name = std::move(name);
    v.file_off = file_off;
    v.ino = ino;
    v.prot4k.assign(len / K4, (uint8_t)prot);
    vmas_.emplace(addr, std::move(v));
    // the slack after `len` up to hlen stays ours (unused) until the last VMA in the host page goes
    return (int64_t)addr;
}

int64_t AddressSpace::unmap(uint64_t addr, uint64_t len) {
    if (addr & (K4 - 1)) return -lx::einval;
    if (len == 0) return -lx::einval;
    len = lx::PageUp(len);
    std::lock_guard<std::mutex> lk(mu_);
    erase_range_locked(addr, addr + len);
    release_uncovered_host_pages(addr, addr + len);
    invalidate(addr, addr + len);
    return 0;
}

int64_t AddressSpace::protect(uint64_t addr, uint64_t len, int prot) {
    if (addr & (K4 - 1)) return -lx::einval;
    len = lx::PageUp(len);
    std::lock_guard<std::mutex> lk(mu_);
    // every page must be mapped (Linux returns ENOMEM otherwise)
    uint64_t a = addr;
    while (a < addr + len) {
        auto it = vmas_.upper_bound(a);
        if (it == vmas_.begin()) return -lx::enomem;
        --it;
        if (it->second.end <= a) return -lx::enomem;
        Vma& v = it->second;
        uint64_t e = std::min(v.end, addr + len);
        for (uint64_t p = a; p < e; p += K4) v.prot4k[(p - v.start) / K4] = (uint8_t)prot;
        a = e;
    }
    apply_host_prot(addr, addr + len);
    // A page that becomes writable may be rewritten under FEX's translations
    // (no SMC tracking yet): drop whatever was translated from it.
    if (prot & lx::prot_write) invalidate(addr, addr + len);
    return 0;
}

bool AddressSpace::copy_in(uint64_t addr, const void* src, size_t len) {
    if (len == 0) return true;
    std::lock_guard<std::mutex> lk(mu_);
    // must be covered
    uint64_t a = lx::PageDown(addr), end = lx::PageUp(addr + len);
    for (uint64_t p = a; p < end;) {
        auto it = vmas_.upper_bound(p);
        if (it == vmas_.begin() || std::prev(it)->second.end <= p) return false;
        p = std::prev(it)->second.end;
    }
    if (!make_writable(addr, addr + len)) return false;
    memcpy(reinterpret_cast<void*>(addr), src, len);
    apply_host_prot(addr, addr + len);
    invalidate(a, end);
    return true;
}

bool AddressSpace::zero(uint64_t addr, size_t len) {
    if (len == 0) return true;
    std::lock_guard<std::mutex> lk(mu_);
    if (!make_writable(addr, addr + len)) return false;
    memset(reinterpret_cast<void*>(addr), 0, len);
    apply_host_prot(addr, addr + len);
    invalidate(lx::PageDown(addr), lx::PageUp(addr + len));
    return true;
}

}  // namespace rlk
