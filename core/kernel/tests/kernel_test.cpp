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
#include "log.h"
#include "mm.h"
#include "pipe.h"
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

int main() {
    test_normalize();
    test_address_space();
    test_fds();
    test_waitq();
    test_pipe();
    test_futex();
    test_loader_images();
    test_loader_host_binary();
    if (g_failures) {
        fprintf(stderr, "kernel_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("kernel_test: all ok (host page %llu, min addr 0x%llx)\n", (unsigned long long)AddressSpace::host_page(),
           (unsigned long long)AddressSpace::min_addr());
    return 0;
}
