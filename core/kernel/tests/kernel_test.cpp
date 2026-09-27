// kernel_test.cpp — host-side unit test of the FEX-independent half of the
// fake kernel (rlkernel_base): address space bookkeeping, the ELF loader
// with in-memory ET_EXEC / ET_DYN(+interp) images (and the host's own
// /bin/true when it is an x86-64 ELF), the fd table, directory listings and
// path normalization. Runs in CI on Linux (host-tests job):
//   cmake -S core -B build/host -DRL_BUILD_TESTS=ON && cmake --build build/host && build/host/kernel/kernel_test
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <atomic>
#include <chrono>
#include <thread>

#include "elf_loader.h"
#include "fds.h"
#include "file_source.h"
#include "futex.h"
#include "linux_abi.h"
#include "locks.h"
#include "log.h"
#include "mm.h"
#include "overlay.h"
#include "pipe.h"
#include "poll.h"
#include "socket.h"
#include "waitq.h"

using namespace rlk;

static int g_failures = 0;
#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
            g_failures++;                                                                  \
        }                                                                                  \
    } while (0)
#define CHECK_MSG(cond, ...)                                                               \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                \
            fprintf(stderr, __VA_ARGS__);                                                  \
            fprintf(stderr, "\n");                                                         \
            g_failures++;                                                                  \
        }                                                                                  \
    } while (0)

// ---- tiny ELF builder ------------------------------------------------------------------

struct SegSpec {
    uint32_t flags;
    uint64_t vaddr, offset, filesz, memsz;
};

// File layout used by every image here: ehdr @0, phdrs @64, interp string
// @0x180, code @0x200 (segment 0 spans [0, 0x210)), data @0x1000.
static std::vector<uint8_t> build_elf(uint16_t type, uint64_t base, const std::vector<uint8_t>& code,
                                      const std::string& interp, bool with_phdr, bool with_data) {
    std::vector<lx::Elf64_Phdr> ph;
    if (with_phdr) ph.push_back({lx::pt_phdr, lx::pf_r, 64, base + 64, base + 64, 0, 0, 8});
    if (!interp.empty()) ph.push_back({lx::pt_interp, lx::pf_r, 0x180, base + 0x180, base + 0x180, interp.size() + 1, interp.size() + 1, 1});
    ph.push_back({lx::pt_load, lx::pf_r | lx::pf_x, 0, base, base, 0x210, 0x210, 0x1000});
    if (with_data) ph.push_back({lx::pt_load, lx::pf_r | lx::pf_w, 0x1000, base + 0x1000, base + 0x1000, 8, 0x2000, 0x1000});
    if (with_phdr) {  // fix up the PT_PHDR size now that the count is known
        ph[0].p_filesz = ph[0].p_memsz = ph.size() * sizeof(lx::Elf64_Phdr);
    }
    std::vector<uint8_t> f(with_data ? 0x1008 : 0x210, 0);
    lx::Elf64_Ehdr eh {};
    memcpy(eh.e_ident, "\x7f" "ELF", 4);
    eh.e_ident[4] = 2;  // ELFCLASS64
    eh.e_ident[5] = 1;  // little endian
    eh.e_ident[6] = 1;
    eh.e_type = type;
    eh.e_machine = lx::em_x86_64;
    eh.e_version = 1;
    eh.e_entry = base + 0x200;
    eh.e_phoff = 64;
    eh.e_ehsize = 64;
    eh.e_phentsize = sizeof(lx::Elf64_Phdr);
    eh.e_phnum = (uint16_t)ph.size();
    memcpy(f.data(), &eh, sizeof eh);
    memcpy(f.data() + 64, ph.data(), ph.size() * sizeof(lx::Elf64_Phdr));
    if (!interp.empty()) memcpy(f.data() + 0x180, interp.c_str(), interp.size() + 1);
    memcpy(f.data() + 0x200, code.data(), code.size());
    if (with_data) memcpy(f.data() + 0x1000, "DATA1234", 8);
    return f;
}

// Reads the auxv table at `addr` into a map.
static std::map<uint64_t, uint64_t> read_auxv(uint64_t addr) {
    std::map<uint64_t, uint64_t> m;
    auto* p = reinterpret_cast<const uint64_t*>(addr);
    for (int i = 0; i < 64; i++) {
        uint64_t k = p[2 * i], v = p[2 * i + 1];
        if (k == lx::at_null) break;
        m[k] = v;
    }
    return m;
}

class HostFileSource final : public FileSource {
public:
    static std::unique_ptr<HostFileSource> open(const std::string& path) {
        int fd = ::open(path.c_str(), host::kORdonly | host::kOCloexec);
        if (fd < 0) return nullptr;
        struct stat st {};
        fstat(fd, &st);
        auto s = std::unique_ptr<HostFileSource>(new HostFileSource());
        s->fd_ = fd;
        s->size_ = (uint64_t)st.st_size;
        s->path_ = path;
        return s;
    }
    ~HostFileSource() override {
        if (fd_ >= 0) ::close(fd_);
    }
    uint64_t size() const override { return size_; }
    int64_t pread(void* buf, size_t len, uint64_t off) override {
        ssize_t n = ::pread(fd_, buf, len, (off_t)off);
        return n < 0 ? -lx::eio : n;
    }
    const std::string& path() const override { return path_; }

private:
    int fd_ = -1;
    uint64_t size_ = 0;
    std::string path_;
};

// ---- tests -----------------------------------------------------------------------------

static void test_normalize() {
    CHECK(NormalizePath("//a/./b/../c/") == "/a/c");
    CHECK(NormalizePath("/") == "/");
    CHECK(NormalizePath("") == "/");
    CHECK(NormalizePath("/../..") == "/");
    CHECK(NormalizePath("usr/bin") == "/usr/bin");
    CHECK(NormalizePath("/usr//lib/x86_64-linux-gnu/../lib64") == "/usr/lib/lib64");
}

static void test_address_space() {
    AddressSpace mm;
    const uint64_t K4 = 4096;
    int64_t a = mm.map(0, 3 * K4, lx::prot_read | lx::prot_write, false, false, "anon");
    CHECK(a > 0);
    CHECK(mm.is_mapped((uint64_t)a, 3 * K4));
    CHECK(!mm.is_mapped((uint64_t)a + 3 * K4, K4));
    uint8_t bytes[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(mm.copy_in((uint64_t)a + K4, bytes, 8));
    CHECK(memcmp(reinterpret_cast<void*>(a + K4), bytes, 8) == 0);
    CHECK(mm.protect((uint64_t)a + K4, K4, lx::prot_none) == 0);
    CHECK(mm.find((uint64_t)a + K4).prot_at((uint64_t)a + K4) == lx::prot_none);
    CHECK(mm.find((uint64_t)a).prot_at((uint64_t)a) == (lx::prot_read | lx::prot_write));
    CHECK(mm.protect((uint64_t)a + 3 * K4, K4, lx::prot_read) == -lx::enomem);  // not mapped
    CHECK(mm.vma_count() == 1);
    std::string maps = mm.maps_text();
    CHECK_MSG(maps.find("anon") != std::string::npos, "%s", maps.c_str());
    CHECK_MSG(maps.find("---p") != std::string::npos, "%s", maps.c_str());  // the PROT_NONE run
    CHECK(mm.unmap((uint64_t)a + K4, K4) == 0);
    CHECK(mm.vma_count() == 2);
    CHECK(mm.is_mapped((uint64_t)a, K4));
    CHECK(!mm.is_mapped((uint64_t)a + K4, K4));
    CHECK(mm.is_mapped((uint64_t)a + 2 * K4, K4));
    CHECK(mm.mapped_bytes() == 2 * K4);

    // MAP_FIXED over our own range replaces it; NOREPLACE refuses.
    CHECK(mm.map((uint64_t)a, K4, lx::prot_read, true, true, "x") == -lx::eexist);
    int64_t r = mm.map((uint64_t)a, 2 * K4, lx::prot_read | lx::prot_write, true, false, "fixed");
    CHECK(r == a);
    CHECK(mm.is_mapped((uint64_t)a, 3 * K4));
    CHECK(mm.find((uint64_t)a).name == "fixed");
    CHECK(mm.find((uint64_t)a + 2 * K4).name == "anon");
    // replaced pages read as zero again
    CHECK(reinterpret_cast<uint8_t*>(a + K4)[0] == 0);

    // Fixed mappings below the host minimum are refused.
    CHECK(mm.map(0x1000, K4, lx::prot_read, true, false, "low") == -lx::enomem);
    CHECK(mm.map(0x1001, K4, lx::prot_read, true, false, "unaligned") == -lx::einval);
    CHECK(mm.map(0, 0, lx::prot_read, false, false, "empty") == -lx::einval);

    // A fixed mapping at a free host address (probe: map, unmap, reuse).
    void* probe = ::mmap(nullptr, 64 * 1024, host::kProtNone, host::kMapPrivate | host::kMapAnon, -1, 0);
    CHECK(probe != host::MapFailed());
    ::munmap(probe, 64 * 1024);
    uint64_t free_addr = AddressSpace::host_up(reinterpret_cast<uint64_t>(probe));
    int64_t fx = mm.map(free_addr, 2 * K4, lx::prot_read | lx::prot_write, true, true, "fresh");
    CHECK_MSG(fx == (int64_t)free_addr, "got %lld", (long long)fx);
    if (fx > 0) {
        reinterpret_cast<uint8_t*>(fx)[0] = 0x5a;
        CHECK(mm.zero((uint64_t)fx, K4));
        CHECK(reinterpret_cast<uint8_t*>(fx)[0] == 0);
    }
    // Conflict: a range the host already gave to someone else.
    void* foreign = ::mmap(nullptr, 64 * 1024, host::kProtRead, host::kMapPrivate | host::kMapAnon, -1, 0);
    CHECK(foreign != host::MapFailed());
    uint64_t faddr = AddressSpace::host_up(reinterpret_cast<uint64_t>(foreign));
    if (faddr + K4 <= reinterpret_cast<uint64_t>(foreign) + 64 * 1024) {
        CHECK(mm.map(faddr, K4, lx::prot_read, true, false, "conflict") == -lx::enomem);
    }
    ::munmap(foreign, 64 * 1024);
}

static void test_fds() {
    FdTable t;
    lx::stat st {};
    FillStat(st, lx::s_ifreg | 0644, 11, 0, 7);
    auto src = std::make_shared<MemFileSource>("/x/hello.txt", std::vector<uint8_t> {'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd'});
    auto f = std::make_shared<RegularFile>(src, st, "/x/hello.txt", lx::o_rdonly);
    CHECK(t.alloc(std::make_shared<DevNullFile>(0), false) == 0);
    CHECK(t.alloc(std::make_shared<DevNullFile>(0), false) == 1);
    CHECK(t.alloc(std::make_shared<DevNullFile>(0), false) == 2);
    CHECK(t.alloc(f, false) == 3);
    CHECK(t.dup(3, 10, true) == 10);
    CHECK(t.dup2(3, 5, false) == 5);
    CHECK(t.dup2(3, 3, false) == 3);
    CHECK(t.get_cloexec(10) == 1);
    CHECK(t.get_cloexec(5) == 0);
    CHECK(t.close(3) == 0);
    CHECK(t.close(3) == -lx::ebadf);
    CHECK(t.get(5) != nullptr);
    CHECK(t.count() == 5);
    t.close_on_exec();
    CHECK(t.get(10) == nullptr);
    CHECK(t.get(5) != nullptr);
    CHECK(t.dup(99, 0, false) == -lx::ebadf);

    // RegularFile semantics
    char buf[16] = {};
    CHECK(f->read(buf, 5) == 5);
    CHECK(memcmp(buf, "hello", 5) == 0);
    CHECK(f->lseek(0, lx::seek_cur) == 5);
    CHECK(f->pread(buf, 3, 6) == 3);
    CHECK(memcmp(buf, "wor", 3) == 0);
    CHECK(f->lseek(0, lx::seek_end) == 11);
    CHECK(f->read(buf, 5) == 0);
    CHECK(f->lseek(-1, lx::seek_set) == -lx::einval);
    lx::stat st2 {};
    CHECK(f->fstat(st2) == 0 && st2.st_size == 11 && st2.st_ino == 7);

    // DirFile getdents64 packing
    std::vector<DirEntryInfo> ents = {{".", lx::dt_dir, 1}, {"..", lx::dt_dir, 1}, {"a", lx::dt_reg, 2}, {"bb", lx::dt_lnk, 3}, {"ccc", lx::dt_dir, 4}};
    lx::stat dst {};
    FillStat(dst, lx::s_ifdir | 0755, 0, 0, 1);
    DirFile d(ents, dst, "/x", lx::o_rdonly | lx::o_directory);
    uint8_t dbuf[256];
    int64_t n = d.getdents64(dbuf, 64);  // fits "." and ".." (24 each) only
    CHECK_MSG(n == 48, "n=%lld", (long long)n);
    n = d.getdents64(dbuf, sizeof dbuf);
    CHECK_MSG(n == 24 + 24 + 24, "n=%lld", (long long)n);  // (19 + len + 1) rounded up to 8
    if (n > 0) {
        size_t off = 0;
        int i = 2;
        while ((int64_t)off < n) {
            lx::dirent64_hdr h {};
            memcpy(&h, dbuf + off, lx::dirent64_hdr_size);
            const char* name = reinterpret_cast<const char*>(dbuf + off + lx::dirent64_hdr_size);
            CHECK(ents[i].name == name);
            CHECK(h.d_type == ents[i].dtype);
            CHECK(h.d_reclen % 8 == 0);
            off += h.d_reclen;
            i++;
        }
        CHECK(i == 5);
    }
    CHECK(d.getdents64(dbuf, sizeof dbuf) == 0);
    CHECK(d.getdents64(dbuf, 8) == 0);  // at EOF even a tiny buffer is fine
    CHECK(d.lseek(0, lx::seek_set) == 0);
    CHECK(d.getdents64(dbuf, 8) == -lx::einval);  // one entry does not fit
}

static void test_loader_images() {
    const uint64_t K4 = 4096;
    // An ET_EXEC address above the host minimum (4 GB on Darwin, 64 KB on Linux).
    const uint64_t exec_base = AddressSpace::min_addr() >= 0x20000000 ? 0x200000000ull : 0x20000000ull;
    const std::vector<uint8_t> code = {0x48, 0x89, 0xf8, 0xf4};  // mov rax,rdi ; hlt

    // ---- ET_EXEC, no interpreter ----
    {
        AddressSpace mm;
        MemFileSource f("/opt/rl/bin/hello-static", build_elf(lx::et_exec, exec_base, code, "", false, true));
        ExecParams p;
        p.argv = {"/opt/rl/bin/hello-static", "arg1"};
        p.envp = {"A=1", "B=two"};
        p.exec_path = "/opt/rl/bin/hello-static";
        p.hwcap = 0x178bfbff;
        for (int i = 0; i < 16; i++) p.random[i] = (uint8_t)(i * 7 + 1);
        ExecLayout l;
        std::string err;
        int rc = ElfLoader::Load(mm, f, p, nullptr, l, err);
        CHECK_MSG(rc == 0, "rc=%d err=%s", rc, err.c_str());
        if (rc == 0) {
            CHECK(!l.has_interp);
            CHECK(l.main.base == 0);
            CHECK(l.entry == exec_base + 0x200);
            CHECK(l.main.phdr == exec_base + 64);
            CHECK(l.main.phnum == 2);
            CHECK(memcmp(reinterpret_cast<void*>(exec_base + 0x200), code.data(), code.size()) == 0);
            CHECK(memcmp(reinterpret_cast<void*>(exec_base + 0x1000), "DATA1234", 8) == 0);
            CHECK(reinterpret_cast<uint8_t*>(exec_base + 0x1000)[8] == 0);       // bss zeroed
            CHECK(reinterpret_cast<uint8_t*>(exec_base + 0x2fff)[0] == 0);
            CHECK(mm.find(exec_base).prot_at(exec_base) == (lx::prot_read | lx::prot_exec));
            CHECK(mm.find(exec_base + 0x1000).prot_at(exec_base + 0x1000) == (lx::prot_read | lx::prot_write));
            CHECK(l.brk_start >= l.main.highest && l.brk_end - l.brk_start == p.brk_reserve);
            CHECK(mm.brk_start == l.brk_start && mm.brk_cur == l.brk_start);
            CHECK(l.stack_top - l.stack_base == p.stack_size);
            CHECK(mm.is_mapped(l.stack_base, p.stack_size));
            CHECK(!mm.is_mapped(l.stack_base - K4, K4) || mm.find(l.stack_base - K4).prot_at(l.stack_base - K4) == lx::prot_none);
            CHECK((l.rsp & 15) == 0);
            auto* sp = reinterpret_cast<const uint64_t*>(l.rsp);
            CHECK(sp[0] == 2);  // argc
            CHECK(strcmp(reinterpret_cast<const char*>(sp[1]), "/opt/rl/bin/hello-static") == 0);
            CHECK(strcmp(reinterpret_cast<const char*>(sp[2]), "arg1") == 0);
            CHECK(sp[3] == 0);
            CHECK(strcmp(reinterpret_cast<const char*>(sp[4]), "A=1") == 0);
            CHECK(strcmp(reinterpret_cast<const char*>(sp[5]), "B=two") == 0);
            CHECK(sp[6] == 0);
            CHECK(l.auxv == l.rsp + 7 * 8);
            auto aux = read_auxv(l.auxv);
            CHECK(aux[lx::at_entry] == exec_base + 0x200);
            CHECK(aux[lx::at_phdr] == exec_base + 64);
            CHECK(aux[lx::at_phnum] == 2);
            CHECK(aux[lx::at_phent] == 56);
            CHECK(aux[lx::at_base] == 0);
            CHECK(aux[lx::at_pagesz] == 4096);
            CHECK(aux[lx::at_hwcap] == 0x178bfbff);
            CHECK(aux[lx::at_uid] == 1000 && aux[lx::at_egid] == 1000);
            CHECK(aux.count(lx::at_sysinfo_ehdr) == 0);
            CHECK(aux[lx::at_random] == l.at_random && memcmp(reinterpret_cast<void*>(l.at_random), p.random.data(), 16) == 0);
            CHECK(strcmp(reinterpret_cast<const char*>(aux[lx::at_execfn]), "/opt/rl/bin/hello-static") == 0);
            CHECK(strcmp(reinterpret_cast<const char*>(aux[lx::at_platform]), "x86_64") == 0);
            CHECK(aux.size() == l.auxv_entries - 1);  // minus AT_NULL
            std::string maps = mm.maps_text();
            CHECK_MSG(maps.find("[stack]") != std::string::npos && maps.find("[heap]") != std::string::npos &&
                          maps.find("hello-static") != std::string::npos,
                      "%s", maps.c_str());
            std::string js = ElfLoader::LayoutJson(l);
            CHECK_MSG(js.find("\"dyn\":false") != std::string::npos, "%s", js.c_str());
        }
    }

    // ---- ET_EXEC below the host minimum: refused with a clear message ----
    {
        AddressSpace mm;
        MemFileSource f("/low", build_elf(lx::et_exec, 0x1000, code, "", false, false));
        ExecParams p;
        p.argv = {"/low"};
        ExecLayout l;
        std::string err;
        int rc = ElfLoader::Load(mm, f, p, nullptr, l, err);
        CHECK_MSG(rc == -lx::enoexec, "rc=%d", rc);
        CHECK_MSG(err.find("relink as PIE") != std::string::npos, "%s", err.c_str());
    }

    // ---- ET_DYN with PT_INTERP + PT_PHDR ----
    {
        AddressSpace mm;
        const std::vector<uint8_t> ld_code = {0x31, 0xc0, 0xf4};  // xor eax,eax ; hlt
        MemFileSource f("/opt/rl/bin/hello-dyn", build_elf(lx::et_dyn, 0, code, "/lib64/fake-ld.so", true, true));
        auto ld_bytes = build_elf(lx::et_dyn, 0, ld_code, "", false, false);
        int opens = 0;
        OpenFileFn open = [&](const std::string& path) -> std::unique_ptr<FileSource> {
            opens++;
            if (path != "/lib64/fake-ld.so") return nullptr;
            return std::make_unique<MemFileSource>(path, ld_bytes);
        };
        ExecParams p;
        p.argv = {"hello-dyn"};
        p.exec_path = "/opt/rl/bin/hello-dyn";
        ExecLayout l;
        std::string err;
        int rc = ElfLoader::Load(mm, f, p, open, l, err);
        CHECK_MSG(rc == 0, "rc=%d err=%s", rc, err.c_str());
        CHECK(opens == 1);
        if (rc == 0) {
            CHECK(l.has_interp);
            CHECK(l.main.is_dyn && l.main.base != 0);
            CHECK(l.main.base >= AddressSpace::min_addr());
            CHECK(l.interp.base != 0 && l.interp.base != l.main.base);
            CHECK(l.entry == l.interp.entry);
            CHECK(l.interp.entry == l.interp.base + 0x200);
            CHECK(l.main.entry == l.main.base + 0x200);
            CHECK(l.main.phdr == l.main.base + 64);
            CHECK(l.main.interp == "/lib64/fake-ld.so");
            CHECK(memcmp(reinterpret_cast<void*>(l.main.base + 0x200), code.data(), code.size()) == 0);
            CHECK(memcmp(reinterpret_cast<void*>(l.interp.base + 0x200), ld_code.data(), ld_code.size()) == 0);
            CHECK(memcmp(reinterpret_cast<void*>(l.main.base + 0x1000), "DATA1234", 8) == 0);
            auto aux = read_auxv(l.auxv);
            CHECK(aux[lx::at_base] == l.interp.base);
            CHECK(aux[lx::at_entry] == l.main.entry);
            CHECK(aux[lx::at_phdr] == l.main.phdr);
            CHECK(aux[lx::at_phnum] == 4);
            CHECK(strcmp(reinterpret_cast<const char*>(aux[lx::at_execfn]), "/opt/rl/bin/hello-dyn") == 0);
            // the whole image span is booked (segment gap included) → 2 images + heap + stack
            CHECK(mm.is_mapped(l.main.lowest, l.main.highest - l.main.lowest));
            CHECK(mm.find(l.main.base).prot_at(l.main.base) == (lx::prot_read | lx::prot_exec));
            CHECK(mm.find(l.main.base + 0x1000).prot_at(l.main.base + 0x1000) == (lx::prot_read | lx::prot_write));
        }
        // missing interpreter
        AddressSpace mm2;
        ExecLayout l2;
        OpenFileFn none = [](const std::string&) -> std::unique_ptr<FileSource> { return nullptr; };
        rc = ElfLoader::Load(mm2, f, p, none, l2, err);
        CHECK_MSG(rc == -lx::enoent, "rc=%d err=%s", rc, err.c_str());
    }

    // ---- garbage ----
    {
        AddressSpace mm;
        MemFileSource f("/etc/passwd", std::vector<uint8_t>(200, 'x'));
        ExecParams p;
        p.argv = {"/etc/passwd"};
        ExecLayout l;
        std::string err;
        CHECK(ElfLoader::Load(mm, f, p, nullptr, l, err) == -lx::enoexec);
        LoadedImage img;
        CHECK(ElfLoader::Inspect(f, img, err) == -lx::enoexec);
    }
}

// The host's own /bin/true through the real ld.so path when the host is x86-64 Linux.
static void test_loader_host_binary() {
    const char* candidates[] = {"/bin/true", "/usr/bin/true"};
    for (const char* path : candidates) {
        auto f = HostFileSource::open(path);
        if (!f) continue;
        LoadedImage img;
        std::string err;
        if (ElfLoader::Inspect(*f, img, err) != 0) {
            printf("note: %s is not an x86-64 ELF (%s); skipping the host-binary test\n", path, err.c_str());
            return;
        }
        AddressSpace mm;
        ExecParams p;
        p.argv = {path};
        p.exec_path = path;
        OpenFileFn open = [](const std::string& ip) -> std::unique_ptr<FileSource> { return HostFileSource::open(ip); };
        ExecLayout l;
        int rc = ElfLoader::Load(mm, *f, p, open, l, err);
        CHECK_MSG(rc == 0, "%s: rc=%d err=%s", path, rc, err.c_str());
        if (rc == 0) {
            CHECK(l.has_interp == !img.interp.empty());
            CHECK(l.entry != 0);
            printf("loaded %s: %s\n", path, ElfLoader::LayoutJson(l).c_str());
            printf("%s", mm.maps_text().c_str());
        }
        return;
    }
    printf("note: no /bin/true on this host; skipping the host-binary test\n");
}

static void test_waitq() {
    Waiter w;
    WaitQueue q;
    // a notify between prepare() and wait() is not lost
    uint64_t gen = w.prepare();
    w.notify();
    CHECK(w.wait(gen, 0) == Waiter::Result::Ready);
    // timeout
    gen = w.prepare();
    auto t0 = std::chrono::steady_clock::now();
    CHECK(w.wait(gen, MonotonicNs() + 20 * 1000 * 1000) == Waiter::Result::Timeout);
    CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(15));
    // interruption
    static bool flag = false;
    w.interrupted = [](void*) { return flag; };
    flag = true;
    gen = w.prepare();
    CHECK(w.wait(gen, 0) == Waiter::Result::Interrupted);
    flag = false;
    w.interrupted = nullptr;
    // queue wake with bitsets
    Waiter a, b;
    a.bitset = 1;
    b.bitset = 2;
    q.add(&a);
    q.add(&b);
    uint64_t ga = a.prepare(), gb = b.prepare();
    CHECK(q.wake(SIZE_MAX, 2) == 1);
    CHECK(b.wait(gb, 1) == Waiter::Result::Ready);
    CHECK(a.wait(ga, MonotonicNs() + 5 * 1000 * 1000) == Waiter::Result::Timeout);
    q.remove(&a);
    q.remove(&b);
    CHECK(q.size() == 0);
}

static void test_pipe() {
    std::shared_ptr<PipeFile> rd, wr;
    MakePipe(rd, wr);
    lx::stat st {};
    CHECK(rd->fstat(st) == 0 && (st.st_mode & lx::s_ifmt) == lx::s_ififo);
    CHECK(wr->read(nullptr, 1) == -lx::ebadf);
    CHECK(rd->write("x", 1) == -lx::ebadf);
    // basic transfer + FIONREAD
    CHECK(wr->write("hello", 5) == 5);
    int32_t avail = 0;
    CHECK(rd->ioctl(lx::fionread, reinterpret_cast<uint64_t>(&avail)) == 0 && avail == 5);
    char buf[16] = {};
    CHECK(rd->read(buf, 3) == 3 && memcmp(buf, "hel", 3) == 0);
    CHECK(rd->read(buf, 16) == 2 && memcmp(buf, "lo", 2) == 0);
    // non-blocking empty read
    rd->oflags |= lx::o_nonblock;
    CHECK(rd->read(buf, 1) == -lx::eagain);
    rd->oflags &= ~lx::o_nonblock;
    // a blocked reader is woken by a writer on another thread
    std::atomic<int> got {-1};
    std::thread reader([&] {
        char b[8];
        got = (int)rd->read(b, sizeof b);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(got == -1);
    CHECK(wr->write("abc", 3) == 3);
    reader.join();
    CHECK(got == 3);
    // a large write streams through a 64 KB ring while a reader drains it
    std::vector<uint8_t> big(200000);
    for (size_t i = 0; i < big.size(); i++) big[i] = (uint8_t)(i * 7);
    std::atomic<size_t> drained {0};
    bool mismatch = false;
    std::thread drain([&] {
        std::vector<uint8_t> chunk(4096);
        size_t pos = 0;
        while (pos < big.size()) {
            int64_t n = rd->read(chunk.data(), chunk.size());
            if (n <= 0) break;
            if (memcmp(chunk.data(), big.data() + pos, (size_t)n) != 0) mismatch = true;
            pos += (size_t)n;
        }
        drained = pos;
    });
    CHECK(wr->write(big.data(), big.size()) == (int64_t)big.size());
    drain.join();
    CHECK(drained == big.size());
    CHECK(!mismatch);
    // EOF once the last writer is gone; EPIPE once the last reader is gone
    wr.reset();
    CHECK(rd->read(buf, 1) == 0);
    std::shared_ptr<PipeFile> rd2, wr2;
    MakePipe(rd2, wr2);
    rd2.reset();
    CHECK(wr2->write("x", 1) == -lx::epipe);
}

static void test_futex() {
    auto& ft = FutexTable::get();
    alignas(4) uint32_t word = 1;
    CHECK(ft.wait(&word, 2, 0, ~0u) == -lx::eagain);  // value mismatch
    CHECK(ft.wait(&word, 1, MonotonicNs() + 10 * 1000 * 1000, ~0u) == -lx::etimedout);
    CHECK(ft.wake(&word, 1, ~0u) == 0);
    std::atomic<int> woken {0};
    std::thread waiter([&] {
        int r = ft.wait(&word, 1, MonotonicNs() + 2000 * 1000 * 1000LL, ~0u);
        if (r == 0) woken = 1;
        else woken = -1;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    word = 0;
    int n = 0;
    for (int i = 0; i < 50 && n == 0; i++) {  // the waiter may not be queued yet
        n = ft.wake(&word, 1, ~0u);
        if (!n) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    waiter.join();
    CHECK(n == 1);
    CHECK(woken == 1);
    // requeue: one waiter moved from word to word2, woken there
    alignas(4) uint32_t word2 = 0;
    word = 5;
    std::atomic<int> state {0};
    std::thread w2([&] { state = ft.wait(&word, 5, MonotonicNs() + 2000 * 1000 * 1000LL, ~0u) == 0 ? 1 : -1; });
    for (int i = 0; i < 100 && ft.requeue(&word, 0, 1, &word2, false, 0) == 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    for (int i = 0; i < 100 && ft.wake(&word2, 1, ~0u) == 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    w2.join();
    CHECK(state == 1);
    // wake_op: FUTEX_OP_ADD 1 to word2, wake word2 if old == 0 → one waiter
    word2 = 0;
    std::atomic<int> s3 {0};
    std::thread w3([&] { s3 = ft.wait(&word2, 0, MonotonicNs() + 2000 * 1000 * 1000LL, ~0u) == 0 ? 1 : -1; });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const uint32_t op = (1u << 28) | (0u << 24) | (1u << 12) | 0u;  // ADD 1, CMP_EQ 0
    int n3 = ft.wake_op(&word, 1, &word2, 1, op);
    w3.join();
    CHECK_MSG(n3 == 1 && word2 == 1 && s3 == 1, "n3=%d word2=%u s3=%d", n3, word2, (int)s3);
}

// ---- overlay ---------------------------------------------------------------------------

// A lower tree in memory: directories, files, symlinks.
struct FakeLower final : LowerFs {
    struct Node {
        uint32_t mode;
        std::string content;  // file bytes or link target
    };
    std::map<std::string, Node> nodes;
    int lstat_calls = 0;

    FakeLower() {
        nodes["/"] = {lx::s_ifdir | 0755, ""};
        nodes["/etc"] = {lx::s_ifdir | 0755, ""};
        nodes["/etc/passwd"] = {lx::s_ifreg | 0644, "root:x\n"};
        nodes["/etc/hostname"] = {lx::s_ifreg | 0644, "box\n"};
        nodes["/var"] = {lx::s_ifdir | 0755, ""};
        nodes["/var/run"] = {lx::s_iflnk | 0777, "/run"};
        nodes["/var/lock"] = {lx::s_iflnk | 0777, "../run/lock"};
        nodes["/usr"] = {lx::s_ifdir | 0755, ""};
        nodes["/usr/bin"] = {lx::s_ifdir | 0755, ""};
        nodes["/usr/bin/true"] = {lx::s_ifreg | 0755, "#!/bin/sh\n"};
        nodes["/bin"] = {lx::s_iflnk | 0777, "usr/bin"};
    }
    int lstat(const std::string& path, lx::stat& st) override {
        lstat_calls++;
        auto it = nodes.find(path);
        if (it == nodes.end()) return lx::enoent;
        FillStat(st, it->second.mode, it->second.content.size(), 1000, 5000 + (uint64_t)std::distance(nodes.begin(), it), 0, 0, 0);
        return 0;
    }
    int readlink(const std::string& path, std::string& target) override {
        auto it = nodes.find(path);
        if (it == nodes.end()) return lx::enoent;
        if ((it->second.mode & lx::s_ifmt) != lx::s_iflnk) return lx::einval;
        target = it->second.content;
        return 0;
    }
    int readdir(const std::string& path, std::vector<DirEntryInfo>& out) override {
        auto it = nodes.find(path);
        if (it == nodes.end()) return lx::enoent;
        if ((it->second.mode & lx::s_ifmt) != lx::s_ifdir) return lx::enotdir;
        out.push_back({".", lx::dt_dir, 1});
        out.push_back({"..", lx::dt_dir, 1});
        const std::string prefix = path == "/" ? "/" : path + "/";
        for (auto& [p, n] : nodes) {
            if (p.size() <= prefix.size() || p.compare(0, prefix.size(), prefix) != 0) continue;
            if (p.find('/', prefix.size()) != std::string::npos) continue;
            uint32_t t = n.mode & lx::s_ifmt;
            out.push_back({p.substr(prefix.size()), t == lx::s_ifdir ? lx::dt_dir : (t == lx::s_iflnk ? lx::dt_lnk : lx::dt_reg), 1});
        }
        return 0;
    }
    std::unique_ptr<FileSource> open(const std::string& path, int& err) override {
        auto it = nodes.find(path);
        if (it == nodes.end()) {
            err = lx::enoent;
            return nullptr;
        }
        if ((it->second.mode & lx::s_ifmt) == lx::s_ifdir) {
            err = lx::eisdir;
            return nullptr;
        }
        return std::make_unique<MemFileSource>(path, std::vector<uint8_t>(it->second.content.begin(), it->second.content.end()));
    }
};

static bool has_name(const std::vector<DirEntryInfo>& ents, const std::string& name) {
    for (auto& e : ents) {
        if (e.name == name) return true;
    }
    return false;
}

static std::string read_all(FileSource& f) {
    std::string s(f.size(), 0);
    if (!s.empty()) ReadAll(f, 0, s.data(), s.size());
    return s;
}

static void test_overlay() {
    FakeLower lower;
    Overlay ov(&lower);
    ov.mkdir_boot("/tmp", 01777, true);
    ov.mkdir_boot("/run", 0755, true);
    ov.mkdir_boot("/run/lock", 01777, true);
    ov.mkdir_boot("/dev/shm", 01777, true);

    lx::stat st {};
    CHECK(ov.stat("/etc/passwd", true, st) == 0 && st.st_size == 7 && (st.st_mode & lx::s_ifmt) == lx::s_ifreg);
    CHECK(ov.stat("/tmp", true, st) == 0 && (st.st_mode & lx::s_ifmt) == lx::s_ifdir && (st.st_mode & 07777) == 01777);
    CHECK(ov.stat("/dev/shm", true, st) == 0 && (st.st_mode & lx::s_ifmt) == lx::s_ifdir);
    CHECK(ov.stat("/nope", true, st) == lx::enoent);
    CHECK(ov.stat("/etc/passwd/x", true, st) == lx::enotdir);
    std::vector<DirEntryInfo> ents;
    CHECK(ov.readdir("/", ents) == 0 && has_name(ents, "etc") && has_name(ents, "tmp") && has_name(ents, "run") &&
          has_name(ents, "dev") && has_name(ents, "usr"));
    // symlinks in the lower tree, absolute and relative
    CHECK(ov.stat("/bin/true", true, st) == 0 && (st.st_mode & 0111));
    CHECK(ov.stat("/var/run", false, st) == 0 && (st.st_mode & lx::s_ifmt) == lx::s_iflnk);
    CHECK(ov.stat("/var/run", true, st) == 0 && (st.st_mode & lx::s_ifmt) == lx::s_ifdir);
    CHECK(ov.stat("/var/lock", true, st) == 0 && (st.st_mode & 07777) == 01777);

    // create + write + read back through the upper layer
    Lookup l;
    CHECK(ov.create("/tmp/a", 0644, true, l) == 0 && l.upper && !l.lower);
    CHECK(ov.create("/tmp/a", 0644, true, l) == lx::eexist);
    CHECK(ov.create("/tmp/a", 0644, false, l) == 0 && l.upper);
    {
        TmpFile f(l.upper, "/tmp/a", lx::o_rdwr);
        CHECK(f.write("hello", 5) == 5);
        CHECK(f.lseek(0, lx::seek_set) == 0);
        char buf[16] = {};
        CHECK(f.read(buf, sizeof buf) == 5 && memcmp(buf, "hello", 5) == 0);
        CHECK(f.pwrite("HE", 2, 0) == 2 && f.pread(buf, 5, 0) == 5 && memcmp(buf, "HEllo", 5) == 0);
        CHECK(f.lseek(0, lx::seek_end) == 5);
        CHECK(f.ftruncate(3) == 0);
        lx::stat fs {};
        CHECK(f.fstat(fs) == 0 && fs.st_size == 3 && fs.st_dev != 0x801);
        TmpFile ap(l.upper, "/tmp/a", lx::o_wronly | lx::o_append);
        CHECK(ap.write("!!", 2) == 2);
        CHECK(f.pread(buf, 16, 0) == 5 && memcmp(buf, "HEl!!", 5) == 0);
    }
    CHECK(ov.stat("/tmp/a", true, st) == 0 && st.st_size == 5);
    ents.clear();
    CHECK(ov.readdir("/tmp", ents) == 0 && has_name(ents, "a") && has_name(ents, ".") && ents.size() == 3);
    int err = 0;
    auto src = ov.open_source("/tmp/a", err);
    CHECK(src && read_all(*src) == "HEl!!");

    // copy-up of a lower file
    std::shared_ptr<TmpNode> n;
    CHECK(ov.for_write("/etc/passwd", true, n) == 0 && n && n->kind == TmpNode::Reg);
    {
        TmpFile f(n, "/etc/passwd", lx::o_wronly | lx::o_append);
        CHECK(f.write("user:x\n", 7) == 7);
    }
    CHECK(ov.stat("/etc/passwd", true, st) == 0 && st.st_size == 14);
    src = ov.open_source("/etc/passwd", err);
    CHECK(src && read_all(*src) == "root:x\nuser:x\n");
    CHECK(lower.nodes["/etc/passwd"].content == "root:x\n");  // the lower tree never changes
    ents.clear();
    CHECK(ov.readdir("/etc", ents) == 0 && has_name(ents, "passwd") && has_name(ents, "hostname"));
    size_t count_passwd = 0;
    for (auto& e : ents) count_passwd += e.name == "passwd";
    CHECK(count_passwd == 1);  // merged, not duplicated

    // whiteouts
    CHECK(ov.unlink("/etc/hostname") == 0);
    CHECK(ov.stat("/etc/hostname", true, st) == lx::enoent);
    ents.clear();
    CHECK(ov.readdir("/etc", ents) == 0 && !has_name(ents, "hostname"));
    CHECK(ov.create("/etc/hostname", 0644, true, l) == 0 && l.upper);
    CHECK(ov.stat("/etc/hostname", true, st) == 0 && st.st_size == 0);
    CHECK(ov.unlink("/etc") == lx::eisdir);
    CHECK(ov.rmdir("/etc") == lx::enotempty);
    CHECK(ov.unlink("/etc/hostname") == 0 && ov.unlink("/etc/passwd") == 0);
    CHECK(ov.rmdir("/etc") == 0);
    CHECK(ov.stat("/etc", true, st) == lx::enoent);
    CHECK(ov.mkdir("/etc", 0755) == 0);
    ents.clear();
    CHECK(ov.readdir("/etc", ents) == 0 && ents.size() == 2);  // opaque: the lower files stay hidden

    // directories
    CHECK(ov.mkdir("/tmp/d", 0700) == 0 && ov.mkdir("/tmp/d", 0700) == lx::eexist);
    CHECK(ov.mkdir("/nodir/x", 0700) == lx::enoent);
    CHECK(ov.rmdir("/tmp") == lx::enotempty);
    CHECK(ov.rmdir("/usr/bin") == lx::enotempty);
    CHECK(ov.rmdir("/tmp/d") == 0 && ov.stat("/tmp/d", true, st) == lx::enoent);
    CHECK(ov.mkdir("/usr/bin/newdir", 0755) == 0);  // an upper dir inside a lower dir
    ents.clear();
    CHECK(ov.readdir("/usr/bin", ents) == 0 && has_name(ents, "true") && has_name(ents, "newdir"));

    // symlinks in the upper layer, lower symlink leading into the upper layer
    CHECK(ov.symlink("/tmp/a", "/tmp/l") == 0);
    CHECK(ov.stat("/tmp/l", true, st) == 0 && st.st_size == 5);
    CHECK(ov.stat("/tmp/l", false, st) == 0 && (st.st_mode & lx::s_ifmt) == lx::s_iflnk);
    std::string target;
    CHECK(ov.readlink("/tmp/l", target) == 0 && target == "/tmp/a");
    CHECK(ov.readlink("/tmp/a", target) == lx::einval);
    CHECK(ov.symlink("/tmp/loop", "/tmp/loop") == 0 && ov.stat("/tmp/loop", true, st) == lx::eloop);
    CHECK(ov.create("/var/run/x.pid", 0644, true, l) == 0 && l.canon == "/run/x.pid");
    CHECK(ov.stat("/run/x.pid", true, st) == 0);
    CHECK(ov.create("/var/lock/y", 0644, true, l) == 0 && ov.stat("/run/lock/y", true, st) == 0);

    // rename / link / mknod / truncate / chmod / utimens
    CHECK(ov.rename("/tmp/a", "/tmp/b", 0) == 0);
    CHECK(ov.stat("/tmp/a", true, st) == lx::enoent && ov.stat("/tmp/b", true, st) == 0 && st.st_size == 5);
    CHECK(ov.rename("/usr/bin/true", "/tmp/true", 0) == 0);  // lower file: copy-up + whiteout
    CHECK(ov.stat("/usr/bin/true", true, st) == lx::enoent);
    CHECK(ov.stat("/tmp/true", true, st) == 0 && (st.st_mode & 07777) == 0755 && st.st_size == 10);
    CHECK(ov.rename("/usr", "/tmp/usr", 0) == lx::exdev);  // merged directory
    CHECK(ov.create("/tmp/c", 0644, true, l) == 0);
    CHECK(ov.rename("/tmp/c", "/tmp/b", lx::rename_noreplace) == lx::eexist);
    CHECK(ov.rename("/tmp/c", "/tmp/b", 0) == 0 && ov.stat("/tmp/b", true, st) == 0 && st.st_size == 0);
    CHECK(ov.link("/tmp/true", "/tmp/true2") == 0);
    CHECK(ov.stat("/tmp/true", true, st) == 0 && st.st_nlink == 2);
    CHECK(ov.unlink("/tmp/true2") == 0 && ov.stat("/tmp/true", true, st) == 0 && st.st_nlink == 1);
    std::shared_ptr<TmpNode> sock;
    CHECK(ov.mknod("/tmp/.X11-unix", lx::s_ifdir, sock) == lx::eperm);
    CHECK(ov.mkdir("/tmp/.X11-unix", 01777) == 0);
    CHECK(ov.mknod("/tmp/.X11-unix/X0", lx::s_ifsock | 0777, sock) == 0 && sock && sock->kind == TmpNode::Sock);
    CHECK(ov.mknod("/tmp/.X11-unix/X0", lx::s_ifsock | 0777, sock) == lx::eexist);
    CHECK(ov.stat("/tmp/.X11-unix/X0", true, st) == 0 && (st.st_mode & lx::s_ifmt) == lx::s_ifsock);
    CHECK(ov.unlink("/tmp/.X11-unix/X0") == 0);
    std::shared_ptr<TmpNode> fifo;
    CHECK(ov.mknod("/tmp/fifo", lx::s_ififo | 0600, fifo) == 0 && fifo->fifo);
    CHECK(ov.truncate("/tmp/true", 2) == 0 && ov.stat("/tmp/true", true, st) == 0 && st.st_size == 2);
    CHECK(ov.chmod("/tmp/true", true, 0600) == 0 && ov.stat("/tmp/true", true, st) == 0 && (st.st_mode & 07777) == 0600);
    CHECK(ov.chmod("/usr/bin", true, 0700) == 0 && ov.stat("/usr/bin", true, st) == 0 && (st.st_mode & 07777) == 0700);
    ents.clear();
    CHECK(ov.readdir("/usr/bin", ents) == 0 && has_name(ents, "newdir") && !has_name(ents, "true"));
    CHECK(ov.utimens("/tmp/true", true, 5, 7) == 0 && ov.stat("/tmp/true", true, st) == 0 && st.st_mtim.tv_sec == 7 &&
          st.st_atim.tv_sec == 5);
    CHECK(ov.chown("/tmp/true", true, 1000, (uint32_t)-1) == 0 && ov.stat("/tmp/true", true, st) == 0 &&
          st.st_uid == 1000 && st.st_gid == 0);  // copied up from a root-owned lower file; gid untouched
    // O_CREAT on a read-only lower name that was never touched must not copy
    CHECK(ov.create("/usr/bin", 0644, false, l) == 0 && l.upper && l.upper->kind == TmpNode::Dir);
}

// ---- shared file mappings (MAP_SHARED aliases of an upper file) ---------------------

static void test_shared_store() {
    FakeLower lower;
    Overlay ov(&lower);
    ov.mkdir_boot("/tmp", 01777, true);
    Lookup l;
    CHECK(ov.create("/tmp/shm", 0600, true, l) == 0 && l.upper);
    TmpFile f(l.upper, "/tmp/shm", lx::o_rdwr);
    CHECK(f.write("hello world", 11) == 11);
    const uint64_t hp = AddressSpace::host_page();
    AddressSpace mm;
    const int rw = lx::prot_read | lx::prot_write;
    int64_t a = mm.map(0, hp, rw, false, false, "/tmp/shm");
    CHECK(a > 0);
    {
        std::lock_guard<std::mutex> lk(l.upper->data->mu);
        CHECK(l.upper->data->ensure_store((size_t)hp));
        CHECK(l.upper->data->size() == 11);  // moving into the store keeps the contents
    }
    CHECK(mm.alias_shared((uint64_t)a, hp, l.upper->data->store.get(), 0, rw));
    CHECK(memcmp(reinterpret_cast<void*>(a), "hello world", 11) == 0);
    memcpy(reinterpret_cast<void*>(a), "HELLO", 5);  // a guest store through the mapping
    char buf[32] = {};
    CHECK(f.pread(buf, 11, 0) == 11 && memcmp(buf, "HELLO world", 11) == 0);  // read() sees it
    CHECK(f.pwrite("!", 1, 10) == 1 && reinterpret_cast<char*>(a)[10] == '!');  // and the reverse
    // a second mapping (another process's view) shares the bytes
    int64_t b = mm.map(0, hp, rw, false, false, "/tmp/shm");
    CHECK(b > 0 && mm.alias_shared((uint64_t)b, hp, l.upper->data->store.get(), 0, rw));
    reinterpret_cast<char*>(b)[0] = 'J';
    CHECK(reinterpret_cast<char*>(a)[0] == 'J');
    // a fork snapshot leaves shared pages alone: the child's writes stay
    mm.push_snapshot();
    reinterpret_cast<char*>(b)[1] = 'Z';
    mm.pop_snapshot();
    CHECK(reinterpret_cast<char*>(a)[1] == 'Z' && f.pread(buf, 2, 0) == 2 && memcmp(buf, "JZ", 2) == 0);
    // MAP_FIXED anonymous memory over the alias must not zero the file
    CHECK(mm.map((uint64_t)a, 4096, rw, true, false, "anon") == a);
    CHECK(reinterpret_cast<char*>(a)[0] == 0);
    CHECK(f.pread(buf, 2, 0) == 2 && memcmp(buf, "JZ", 2) == 0);
    CHECK(mm.unmap((uint64_t)b, hp) == 0);
    CHECK(f.pread(buf, 2, 0) == 2 && memcmp(buf, "JZ", 2) == 0);  // the store outlives its aliases
    // growth: within the store's capacity, then ENOSPC
    CHECK(f.ftruncate(hp * 2) == 0);
    CHECK(f.ftruncate(64u << 20) == -lx::enospc);
    // misaligned addresses/offsets are refused (the caller falls back to a private copy)
    int64_t c2 = mm.map(0, hp, rw, false, false, "/tmp/shm");
    CHECK(c2 > 0);
    CHECK(!mm.alias_shared((uint64_t)c2, hp, l.upper->data->store.get(), hp + 1, rw));
    CHECK(!mm.alias_shared((uint64_t)c2 + 8, hp, l.upper->data->store.get(), 0, rw));
    CHECK(!mm.alias_shared((uint64_t)c2, hp, l.upper->data->store.get(), l.upper->data->store->capacity(), rw));
}

// ---- poll / eventfd / epoll ---------------------------------------------------------

static void test_poll() {
    std::shared_ptr<PipeFile> rd, wr;
    MakePipe(rd, wr);
    std::vector<PollEntry> pe;
    pe.push_back({rd, lx::pollin, 0});
    pe.push_back({wr, lx::pollout, 0});
    CHECK(DoPoll(pe, 0, true) == 1 && pe[0].revents == 0 && pe[1].revents == lx::pollout);
    CHECK(wr->write("x", 1) == 1);
    CHECK(DoPoll(pe, 0, true) == 2 && (pe[0].revents & lx::pollin));
    char c;
    CHECK(rd->read(&c, 1) == 1);
    // timeout
    const int64_t t0 = MonotonicNs();
    CHECK(DoPoll(pe, MonotonicNs() + 30'000'000, false) == 1);  // only POLLOUT is ready → returns at once
    pe.pop_back();
    CHECK(DoPoll(pe, MonotonicNs() + 30'000'000, false) == 0);
    CHECK(MonotonicNs() - t0 >= 25'000'000);
    // a writer wakes a blocked poller
    std::thread t([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        wr->write("y", 1);
    });
    CHECK(DoPoll(pe, MonotonicNs() + 2'000'000'000, false) == 1 && (pe[0].revents & lx::pollin));
    t.join();
    CHECK(rd->read(&c, 1) == 1 && c == 'y');
    // closed write end: POLLHUP on the read end; closed read end: POLLERR on the write end
    std::shared_ptr<PipeFile> rd2, wr2;
    MakePipe(rd2, wr2);
    wr2.reset();
    std::vector<PollEntry> pe2 {{rd2, lx::pollin, 0}};
    CHECK(DoPoll(pe2, 0, true) == 1 && (pe2[0].revents & lx::pollhup));
    pe.clear();  // drops the poll entries' reference to the read end
    rd.reset();
    std::vector<PollEntry> pe3 {{wr, lx::pollout, 0}};
    CHECK(DoPoll(pe3, 0, true) == 1 && (pe3[0].revents & lx::pollerr));
    // bad fd
    std::vector<PollEntry> pe4 {{nullptr, lx::pollin, 0}};
    CHECK(DoPoll(pe4, 0, true) == 1 && pe4[0].revents == lx::pollnval);

    // eventfd
    auto ef = std::make_shared<EventFdFile>(0, lx::efd_nonblock);
    uint64_t v = 0;
    CHECK(ef->read(&v, 8) == -lx::eagain);
    CHECK(ef->poll(nullptr) == lx::pollout);
    v = 3;
    CHECK(ef->write(&v, 8) == 8 && ef->poll(nullptr) == (lx::pollin | lx::pollout));
    v = 0;
    CHECK(ef->read(&v, 8) == 8 && v == 3);
    auto sem = std::make_shared<EventFdFile>(2, lx::efd_semaphore | lx::efd_nonblock);
    CHECK(sem->read(&v, 8) == 8 && v == 1 && sem->read(&v, 8) == 8 && v == 1 && sem->read(&v, 8) == -lx::eagain);
    auto blocking = std::make_shared<EventFdFile>(0, 0);
    std::thread t2([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        uint64_t one = 1;
        blocking->write(&one, 8);
    });
    CHECK(blocking->read(&v, 8) == 8 && v == 1);
    t2.join();

    // epoll
    auto ep = std::make_shared<EpollFile>();
    std::shared_ptr<PipeFile> rd3, wr3;
    MakePipe(rd3, wr3);
    CHECK(ep->ctl(lx::epoll_ctl_add, 5, rd3, lx::epollin, 0x55) == 0);
    CHECK(ep->ctl(lx::epoll_ctl_add, 5, rd3, lx::epollin, 0x55) == -lx::eexist);
    CHECK(ep->ctl(lx::epoll_ctl_add, 6, ef, lx::epollin | lx::epolloneshot, 0x66) == 0);
    CHECK(ep->ctl(lx::epoll_ctl_add, 7, sem, lx::epollout | lx::epollet, 0x77) == 0);
    lx::epoll_event evs[8];
    // sem (POLLOUT, edge-triggered) reports once; the pipe and the eventfd are not readable
    int n = (int)ep->wait(evs, 8, 0, true);
    CHECK_MSG(n == 1 && evs[0].data == 0x77, "n=%d", n);
    CHECK(ep->wait(evs, 8, 0, true) == 0);  // no new edge
    CHECK(wr3->write("z", 1) == 1);
    v = 1;
    CHECK(ef->write(&v, 8) == 8);
    n = (int)ep->wait(evs, 8, 0, true);
    CHECK_MSG(n == 2, "n=%d", n);
    CHECK(ep->wait(evs, 8, 0, true) == 1 && evs[0].data == 0x55);  // oneshot fired, pipe still readable
    CHECK(ep->ctl(lx::epoll_ctl_mod, 6, ef, lx::epollin, 0x66) == 0);
    CHECK(ep->wait(evs, 8, 0, true) == 2);
    CHECK(ep->poll(nullptr) == lx::pollin);
    CHECK(ep->ctl(lx::epoll_ctl_del, 6, nullptr, 0, 0) == 0 && ep->ctl(lx::epoll_ctl_del, 6, nullptr, 0, 0) == -lx::enoent);
    CHECK(rd3->read(&c, 1) == 1);
    // blocked epoll_wait woken by a write, then timeout
    std::thread t3([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        wr3->write("w", 1);
    });
    CHECK(ep->wait(evs, 8, MonotonicNs() + 2'000'000'000, false) == 1 && evs[0].data == 0x55);
    t3.join();
    CHECK(rd3->read(&c, 1) == 1);
    CHECK(ep->wait(evs, 8, MonotonicNs() + 20'000'000, false) == 0);
    // Edge-triggered readiness is "woken since the last scan", not a mask
    // change (result 006: Xvfb drained a client and its next request arrived
    // before the next epoll_wait — a mask comparison never reported it again).
    std::shared_ptr<PipeFile> rd4, wr4;
    MakePipe(rd4, wr4);
    char b4[8];
    CHECK(ep->ctl(lx::epoll_ctl_add, 8, rd4, lx::epollin | lx::epollet, 0x88) == 0);
    CHECK(ep->wait(evs, 8, 0, true) == 0);  // added while empty: nothing
    CHECK(wr4->write("a", 1) == 1);
    CHECK(ep->wait(evs, 8, 0, true) == 1 && evs[0].data == 0x88);
    CHECK(ep->wait(evs, 8, 0, true) == 0);  // not drained, no new wake-up: no repeat
    CHECK(wr4->write("b", 1) == 1);
    CHECK(ep->wait(evs, 8, 0, true) == 1);  // more data is a new edge
    CHECK(rd4->read(b4, sizeof b4) == 2);   // drained with no scan in between …
    CHECK(wr4->write("c", 1) == 1);         // … and refilled before the next one
    CHECK(ep->wait(evs, 8, 0, true) == 1 && evs[0].data == 0x88);  // the lost case
    CHECK(ep->poll(nullptr) == 0);          // consumed: the set is not readable until the next wake-up
    CHECK(ep->ctl(lx::epoll_ctl_mod, 8, rd4, lx::epollin | lx::epollet, 0x88) == 0);
    CHECK(ep->wait(evs, 8, 0, true) == 1);  // MOD re-reports the current state (data still unread)
    CHECK(ep->ctl(lx::epoll_ctl_del, 8, nullptr, 0, 0) == 0);
    // a file closed everywhere drops out of the interest list
    CHECK(ep->size() == 2);
    rd3.reset();
    wr3.reset();
    CHECK(ep->wait(evs, 8, 0, true) == 0 && ep->size() == 1);
}

// ---- unix sockets -----------------------------------------------------------------

static void test_socket() {
    SockCreds me {42, 1000, 1000};
    std::shared_ptr<UnixSocket> a, b;
    UnixSocket::pair(lx::sock_stream, me, a, b);
    auto fa = std::make_shared<SocketFile>(a, 0);
    auto fb = std::make_shared<SocketFile>(b, 0);
    CHECK(fa->write("hello", 5) == 5);
    char buf[64] = {};
    CHECK(fb->read(buf, sizeof buf) == 5 && memcmp(buf, "hello", 5) == 0);
    CHECK(a->poll(nullptr) & lx::pollout);
    CHECK(!(b->poll(nullptr) & lx::pollin));
    // fd passing: one set per recv, data before it stays separate
    std::vector<std::shared_ptr<OpenFile>> fds {std::make_shared<DevNullFile>(0), std::make_shared<DevNullFile>(1)};
    CHECK(a->send((const uint8_t*)"ab", 2, {}, 0, nullptr, false) == 2);
    CHECK(a->send((const uint8_t*)"cd", 2, fds, 0, nullptr, false) == 2);
    CHECK(a->send((const uint8_t*)"ef", 2, {}, 0, nullptr, false) == 2);
    RecvInfo info;
    int64_t n = b->recv((uint8_t*)buf, sizeof buf, 0, false, &info);
    CHECK_MSG(n == 4 && memcmp(buf, "abcd", 4) == 0 && info.fds.size() == 2 && info.creds.pid == 42, "n=%lld fds=%zu", (long long)n, info.fds.size());
    info = RecvInfo {};
    CHECK(b->recv((uint8_t*)buf, sizeof buf, 0, false, &info) == 2 && memcmp(buf, "ef", 2) == 0 && info.fds.empty());
    CHECK(b->recv((uint8_t*)buf, sizeof buf, 0, true, nullptr) == -lx::eagain);
    CHECK(a->peer_creds().pid == 42 && b->has_peer_creds());
    // shutdown(WR) → EOF on the other side; then close → HUP
    CHECK(a->shutdown(lx::shut_wr) == 0);
    CHECK(fb->read(buf, sizeof buf) == 0);
    CHECK(fa->write("x", 1) == -lx::epipe);
    CHECK(fb->write("back", 4) == 4);  // the other direction still works
    CHECK(fa->read(buf, sizeof buf) == 4);
    fa.reset();
    CHECK((b->poll(nullptr) & (lx::pollhup | lx::pollin | lx::pollrdhup)) == (lx::pollhup | lx::pollin | lx::pollrdhup));
    CHECK(fb->write("y", 1) == -lx::epipe);
    CHECK(fb->read(buf, sizeof buf) == 0);

    // listener in the abstract namespace
    auto srv = std::make_shared<UnixSocket>(lx::sock_stream, SockCreds {7, 1000, 1000});
    auto srvf = std::make_shared<SocketFile>(srv, 0);
    CHECK(srv->listen(5) == -lx::einval);  // unbound
    CHECK(srv->bind_abstract("/tmp/.X11-unix/X0") == 0);
    auto dup = std::make_shared<UnixSocket>(lx::sock_stream, me);
    CHECK(dup->bind_abstract("/tmp/.X11-unix/X0") == -lx::eaddrinuse);
    CHECK(srv->name() == "@/tmp/.X11-unix/X0");
    auto cli = std::make_shared<UnixSocket>(lx::sock_stream, SockCreds {9, 1000, 1000});
    auto clif = std::make_shared<SocketFile>(cli, 0);
    CHECK(cli->connect(UnixSocket::find_abstract("/tmp/.X11-unix/X0"), false) == -lx::econnrefused);  // not listening yet
    CHECK(srv->listen(5) == 0);
    CHECK(!(srv->poll(nullptr) & lx::pollin));
    int err = 0;
    CHECK(srv->accept(true, err) == nullptr && err == lx::eagain);
    CHECK(cli->connect(UnixSocket::find_abstract("/tmp/.X11-unix/X0"), false) == 0);
    CHECK(cli->connect(UnixSocket::find_abstract("/tmp/.X11-unix/X0"), false) == -lx::eisconn);
    CHECK(srv->poll(nullptr) & lx::pollin);
    auto conn = srv->accept(true, err);
    CHECK(conn && err == 0);
    auto connf = std::make_shared<SocketFile>(conn, 0);
    CHECK(conn->peer_creds().pid == 9 && cli->peer_creds().pid == 7);
    CHECK(cli->peer_name() == "@/tmp/.X11-unix/X0" && conn->name() == "@/tmp/.X11-unix/X0");
    CHECK(clif->write("req", 3) == 3 && connf->read(buf, sizeof buf) == 3);
    CHECK(connf->write("rep", 3) == 3 && clif->read(buf, sizeof buf) == 3 && memcmp(buf, "rep", 3) == 0);
    // a blocked reader is woken by the peer
    std::thread t([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        connf->write("late", 4);
    });
    CHECK(clif->read(buf, sizeof buf) == 4 && memcmp(buf, "late", 4) == 0);
    t.join();
    // closing the listener with a queued, unaccepted connection resets that client
    auto cli2 = std::make_shared<UnixSocket>(lx::sock_stream, me);
    auto cli2f = std::make_shared<SocketFile>(cli2, lx::sock_nonblock);
    CHECK(cli2->connect(UnixSocket::find_abstract("/tmp/.X11-unix/X0"), true) == 0);
    srvf.reset();
    CHECK(UnixSocket::find_abstract("/tmp/.X11-unix/X0") == nullptr);
    CHECK(cli2f->read(buf, sizeof buf) == 0);
    CHECK(clif->write("still", 5) == 5 && connf->read(buf, sizeof buf) == 5);  // accepted connections survive

    // flow control
    conn->rcvbuf = 8;
    CHECK(cli->send((const uint8_t*)"0123456789", 10, {}, 0, nullptr, true) == 8);
    CHECK(cli->send((const uint8_t*)"x", 1, {}, 0, nullptr, true) == -lx::eagain);
    CHECK(!(cli->poll(nullptr) & lx::pollout));
    CHECK(connf->read(buf, 4) == 4);
    CHECK(cli->poll(nullptr) & lx::pollout);
    std::thread t2([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        char tmp[64];
        connf->read(tmp, sizeof tmp);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        connf->read(tmp, sizeof tmp);
    });
    CHECK(cli->send((const uint8_t*)"abcdefghij", 10, {}, 0, nullptr, false) == 10);  // blocks until drained
    t2.join();

    // dgram: boundaries, truncation, unconnected sends
    std::shared_ptr<UnixSocket> d1, d2;
    UnixSocket::pair(lx::sock_dgram, me, d1, d2);
    CHECK(d1->send((const uint8_t*)"one", 3, {}, 0, nullptr, false) == 3);
    CHECK(d1->send((const uint8_t*)"three", 5, {}, 0, nullptr, false) == 5);
    info = RecvInfo {};
    CHECK(d2->recv((uint8_t*)buf, 2, 0, false, &info) == 2 && info.truncated && memcmp(buf, "on", 2) == 0);
    CHECK(d2->queued_bytes() == 5);
    CHECK(d2->recv((uint8_t*)buf, 2, lx::msg_trunc, false, &info) == 5);
    CHECK(d2->recv((uint8_t*)buf, sizeof buf, 0, true, nullptr) == -lx::eagain);
    auto d3 = std::make_shared<UnixSocket>(lx::sock_dgram, me);
    CHECK(d3->send((const uint8_t*)"x", 1, {}, 0, nullptr, true) == -lx::enotconn);
    CHECK(d3->send((const uint8_t*)"to-d2", 5, {}, 0, d2, true) == 5);
    info = RecvInfo {};
    CHECK(d2->recv((uint8_t*)buf, sizeof buf, 0, true, &info) == 5 && info.from.empty());
    CHECK(d3->poll(nullptr) & lx::pollout);

    // a filesystem name through the overlay
    FakeLower lower;
    Overlay ov(&lower);
    ov.mkdir_boot("/tmp", 01777, true);
    std::shared_ptr<TmpNode> node;
    CHECK(ov.mknod("/tmp/sock", lx::s_ifsock | 0777, node) == 0);
    auto fsrv = std::make_shared<UnixSocket>(lx::sock_stream, me);
    CHECK(fsrv->bind_path("/tmp/sock", node) == 0 && fsrv->listen(1) == 0);
    Lookup l;
    CHECK(ov.lookup("/tmp/sock", true, l) == 0 && l.upper && l.upper->socket.lock() == fsrv);
    CHECK(fsrv->name() == "/tmp/sock");
}

// ---- record locks ---------------------------------------------------------------------

static void test_locks() {
    auto& lt = LockTable::get();
    const LockKey k {0x14, 77};
    const uint64_t inf = UINT64_MAX;
    CHECK(lt.set(k, 0, inf, lx::f_wrlck, 1, nullptr, false) == 0);
    CHECK(lt.set(k, 0, inf, lx::f_wrlck, 2, nullptr, false) == -lx::eagain);
    CHECK(lt.set(k, 0, inf, lx::f_rdlck, 2, nullptr, false) == -lx::eagain);
    lx::flock fl {};
    CHECK(lt.get(k, 10, 20, lx::f_wrlck, 2, nullptr, fl) == 0 && fl.l_type == lx::f_wrlck && fl.l_pid == 1 && fl.l_len == 0);
    CHECK(lt.get(k, 10, 20, lx::f_wrlck, 1, nullptr, fl) == 0 && fl.l_type == lx::f_unlck);  // own lock
    CHECK(lt.set(k, 0, inf, lx::f_unlck, 1, nullptr, false) == 0 && lt.count() == 0);
    CHECK(lt.set(k, 0, inf, lx::f_rdlck, 2, nullptr, false) == 0);
    CHECK(lt.set(k, 0, inf, lx::f_rdlck, 3, nullptr, false) == 0);  // shared readers
    CHECK(lt.set(k, 0, 10, lx::f_wrlck, 3, nullptr, false) == -lx::eagain);
    lt.release_pid(2);
    CHECK(lt.set(k, 0, 10, lx::f_wrlck, 3, nullptr, false) == 0);  // upgrade of its own range
    CHECK(lt.count() == 2);
    lt.release_posix(k, 3);
    CHECK(lt.count() == 0);
    // splitting
    CHECK(lt.set(k, 0, 100, lx::f_wrlck, 1, nullptr, false) == 0);
    CHECK(lt.set(k, 40, 60, lx::f_unlck, 1, nullptr, false) == 0 && lt.count() == 2);
    CHECK(lt.get(k, 45, 50, lx::f_wrlck, 2, nullptr, fl) == 0 && fl.l_type == lx::f_unlck);
    CHECK(lt.get(k, 0, 10, lx::f_wrlck, 2, nullptr, fl) == 0 && fl.l_type == lx::f_wrlck && fl.l_start == 0 && fl.l_len == 40);
    CHECK(lt.set(k, 40, 60, lx::f_wrlck, 1, nullptr, false) == 0 && lt.count() == 1);  // merged back
    // blocking waiter released by an unlock
    std::thread t([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        lt.release_pid(1);
    });
    CHECK(lt.set(k, 0, inf, lx::f_wrlck, 2, nullptr, true) == 0);
    t.join();
    lt.release_pid(2);
    // OFD locks: owned by the description, conflict within one pid
    int ofd_a, ofd_b;
    CHECK(lt.set(k, 0, inf, lx::f_wrlck, 5, &ofd_a, false) == 0);
    CHECK(lt.set(k, 0, inf, lx::f_wrlck, 5, &ofd_b, false) == -lx::eagain);
    CHECK(lt.get(k, 0, 1, lx::f_wrlck, 5, &ofd_b, fl) == 0 && fl.l_type == lx::f_wrlck && fl.l_pid == -1);
    lt.release_ofd(&ofd_a);
    CHECK(lt.set(k, 0, inf, lx::f_wrlck, 5, &ofd_b, false) == 0);
    lt.release_ofd(&ofd_b);
    // flock
    CHECK(lt.flock(k, &ofd_a, lx::lock_ex) == 0);
    CHECK(lt.flock(k, &ofd_b, lx::lock_sh | lx::lock_nb) == -lx::eagain);
    CHECK(lt.flock(k, &ofd_a, lx::lock_sh) == 0);  // downgrade
    CHECK(lt.flock(k, &ofd_b, lx::lock_sh | lx::lock_nb) == 0);
    CHECK(lt.flock(k, &ofd_a, lx::lock_un) == 0 && lt.flock(k, &ofd_b, lx::lock_un) == 0);
    CHECK(lt.count() == 0);
}

int main() {
    const bool verbose = getenv("KERNEL_TEST_VERBOSE") != nullptr;
#define RUN(t)                                          \
    do {                                                \
        if (verbose) fprintf(stderr, "-- %s\n", #t);    \
        t();                                            \
    } while (0)
    RUN(test_normalize);
    RUN(test_address_space);
    RUN(test_fds);
    RUN(test_waitq);
    RUN(test_pipe);
    RUN(test_futex);
    RUN(test_overlay);
    RUN(test_shared_store);
    RUN(test_poll);
    RUN(test_socket);
    RUN(test_locks);
    RUN(test_loader_images);
    RUN(test_loader_host_binary);
#undef RUN
    if (g_failures) {
        fprintf(stderr, "kernel_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("kernel_test: all ok (host page %llu, min addr 0x%llx)\n", (unsigned long long)AddressSpace::host_page(),
           (unsigned long long)AddressSpace::min_addr());
    return 0;
}
