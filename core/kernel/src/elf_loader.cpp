// elf_loader.cpp — see elf_loader.h.
#include "elf_loader.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "linux_abi.h"
#include "log.h"

namespace rlk {

namespace {

struct Parsed {
    lx::Elf64_Ehdr ehdr {};
    std::vector<lx::Elf64_Phdr> phdrs;
    std::string interp;
};

int parse(FileSource& f, Parsed& p, std::string& err) {
    if (f.size() < sizeof(lx::Elf64_Ehdr)) {
        err = "file too small for an ELF header";
        return -lx::enoexec;
    }
    if (!ReadAll(f, 0, &p.ehdr, sizeof p.ehdr)) {
        err = "cannot read the ELF header";
        return -lx::eio;
    }
    const auto& e = p.ehdr;
    if (memcmp(e.e_ident, "\x7f" "ELF", 4) != 0) {
        err = "not an ELF file";
        return -lx::enoexec;
    }
    if (e.e_ident[4] != 2) {  // ELFCLASS64
        err = "32-bit ELF (not supported: 64-bit only)";
        return -lx::enoexec;
    }
    if (e.e_machine != lx::em_x86_64) {
        err = "e_machine != x86-64";
        return -lx::enoexec;
    }
    if (e.e_type != lx::et_exec && e.e_type != lx::et_dyn) {
        err = "e_type is neither ET_EXEC nor ET_DYN";
        return -lx::enoexec;
    }
    if (e.e_phentsize != sizeof(lx::Elf64_Phdr) || e.e_phnum == 0 || e.e_phnum > 512) {
        err = "bad program header table";
        return -lx::enoexec;
    }
    if (e.e_phoff + (uint64_t)e.e_phnum * sizeof(lx::Elf64_Phdr) > f.size()) {
        err = "program headers beyond the end of the file";
        return -lx::enoexec;
    }
    p.phdrs.resize(e.e_phnum);
    if (!ReadAll(f, e.e_phoff, p.phdrs.data(), p.phdrs.size() * sizeof(lx::Elf64_Phdr))) {
        err = "cannot read the program headers";
        return -lx::eio;
    }
    for (const auto& ph : p.phdrs) {
        if (ph.p_type == lx::pt_interp) {
            if (ph.p_filesz == 0 || ph.p_filesz > 4096 || ph.p_offset + ph.p_filesz > f.size()) {
                err = "bad PT_INTERP";
                return -lx::enoexec;
            }
            std::string s(ph.p_filesz, '\0');
            if (!ReadAll(f, ph.p_offset, s.data(), s.size())) {
                err = "cannot read PT_INTERP";
                return -lx::eio;
            }
            while (!s.empty() && s.back() == '\0') s.pop_back();
            p.interp = s;
        }
    }
    return 0;
}

int prot_of(uint32_t flags) {
    int p = 0;
    if (flags & lx::pf_r) p |= lx::prot_read;
    if (flags & lx::pf_w) p |= lx::prot_write;
    if (flags & lx::pf_x) p |= lx::prot_exec;
    return p;
}

// Copies [off, off+len) of the file to guest address dst through mm.
bool copy_segment(AddressSpace& mm, FileSource& f, uint64_t dst, uint64_t off, uint64_t len, std::string& err) {
    std::vector<uint8_t> buf(std::min<uint64_t>(len, 1u << 20));
    uint64_t done = 0;
    while (done < len) {
        size_t chunk = (size_t)std::min<uint64_t>(buf.size(), len - done);
        int64_t n = f.pread(buf.data(), chunk, off + done);
        if (n <= 0) {
            char m[160];
            snprintf(m, sizeof m, "short read at file offset %llu (%lld)", (unsigned long long)(off + done), (long long)n);
            err = m;
            return false;
        }
        if (!mm.copy_in(dst + done, buf.data(), (size_t)n)) {
            err = "copy into guest memory failed";
            return false;
        }
        done += (uint64_t)n;
    }
    return true;
}

int load_image(AddressSpace& mm, FileSource& f, const Parsed& p, LoadedImage& img, std::string& err) {
    img.path = f.path();
    img.is_dyn = p.ehdr.e_type == lx::et_dyn;
    img.interp = p.interp;
    img.phnum = p.ehdr.e_phnum;

    uint64_t lo = ~0ull, hi = 0;
    for (const auto& ph : p.phdrs) {
        if (ph.p_type != lx::pt_load) continue;
        lo = std::min(lo, lx::PageDown(ph.p_vaddr));
        hi = std::max(hi, lx::PageUp(ph.p_vaddr + ph.p_memsz));
    }
    if (hi <= lo) {
        err = "no PT_LOAD";
        return -lx::enoexec;
    }
    if (!img.is_dyn && lo < AddressSpace::min_addr()) {
        // Guest address == host address, and the host cannot map below
        // min_addr (4 GB hard page zero on tvOS): non-PIE executables linked
        // at 0x400000 can never run here. Relink as PIE (-pie / -static-pie).
        char m[240];
        snprintf(m, sizeof m,
                 "ET_EXEC image at 0x%llx is below the lowest mappable address 0x%llx (4 GB hard page zero): relink as PIE "
                 "(-pie / -static-pie)",
                 (unsigned long long)lo, (unsigned long long)AddressSpace::min_addr());
        err = m;
        return -lx::enoexec;
    }

    uint64_t base = 0;
    if (img.is_dyn) {
        // One reservation for the whole span so the segments' relative
        // layout is preserved; the segments are then re-mapped inside it.
        int64_t r = mm.map(0, hi - lo, lx::prot_none, false, false, img.path);
        if (r < 0) {
            err = "cannot reserve the image span";
            return (int)r;
        }
        base = (uint64_t)r - lo;
    }
    img.base = base;

    for (const auto& ph : p.phdrs) {
        if (ph.p_type != lx::pt_load) continue;
        const uint64_t seg_start = base + lx::PageDown(ph.p_vaddr);
        const uint64_t seg_end = base + lx::PageUp(ph.p_vaddr + ph.p_memsz);
        if (seg_end <= seg_start) continue;
        const int prot = prot_of(ph.p_flags);
        // ET_EXEC segments must land exactly where the file says (noreplace);
        // ET_DYN segments replace parts of our own reservation.
        int64_t r = mm.map(seg_start, seg_end - seg_start, prot, true, !img.is_dyn, img.path,
                           lx::PageDown(ph.p_offset), f.inode());
        if (r < 0) {
            char m[200];
            snprintf(m, sizeof m, "cannot map segment vaddr=0x%llx size=0x%llx at 0x%llx (%s)", (unsigned long long)ph.p_vaddr,
                     (unsigned long long)ph.p_memsz, (unsigned long long)seg_start, ErrnoName((int)-r));
            err = m;
            return (int)r;
        }
        if (ph.p_filesz) {
            if (ph.p_offset + ph.p_filesz > f.size()) {
                err = "segment beyond the end of the file";
                return -lx::enoexec;
            }
            if (!copy_segment(mm, f, base + ph.p_vaddr, ph.p_offset, ph.p_filesz, err)) return -lx::eio;
        }
        // memsz > filesz tail is zero already (fresh mapping)
    }
    img.lowest = base + lo;
    img.highest = base + hi;
    img.entry = base + p.ehdr.e_entry;

    // Program header address: prefer PT_PHDR, else derive from the first
    // PT_LOAD that covers e_phoff.
    img.phdr = 0;
    for (const auto& ph : p.phdrs) {
        if (ph.p_type == lx::pt_phdr) img.phdr = base + ph.p_vaddr;
    }
    if (!img.phdr) {
        for (const auto& ph : p.phdrs) {
            if (ph.p_type == lx::pt_load && ph.p_offset <= p.ehdr.e_phoff && p.ehdr.e_phoff < ph.p_offset + ph.p_filesz) {
                img.phdr = base + ph.p_vaddr + (p.ehdr.e_phoff - ph.p_offset);
                break;
            }
        }
    }
    return 0;
}

}  // namespace

int ElfLoader::Inspect(FileSource& file, LoadedImage& out, std::string& err) {
    Parsed p;
    int rc = parse(file, p, err);
    if (rc) return rc;
    out.path = file.path();
    out.is_dyn = p.ehdr.e_type == lx::et_dyn;
    out.interp = p.interp;
    out.entry = p.ehdr.e_entry;
    out.phnum = p.ehdr.e_phnum;
    return 0;
}

int ElfLoader::Load(AddressSpace& mm, FileSource& main, const ExecParams& params, const OpenFileFn& open, ExecLayout& out,
                    std::string& err) {
    Parsed pm;
    int rc = parse(main, pm, err);
    if (rc) return rc;
    if (params.argv.empty()) {
        err = "empty argv";
        return -lx::einval;
    }

    // Interpreter first (it ends up above the main image like on Linux).
    std::unique_ptr<FileSource> interp_file;
    Parsed pi;
    if (!pm.interp.empty()) {
        interp_file = open ? open(pm.interp) : nullptr;
        if (!interp_file) {
            err = "interpreter not found: " + pm.interp;
            return -lx::enoent;
        }
        std::string ierr;
        rc = parse(*interp_file, pi, ierr);
        if (rc) {
            err = "interpreter " + pm.interp + ": " + ierr;
            return rc;
        }
        if (!pi.interp.empty()) {
            err = "interpreter has an interpreter";
            return -lx::enoexec;
        }
        rc = load_image(mm, *interp_file, pi, out.interp, err);
        if (rc) return rc;
        out.has_interp = true;
    }

    rc = load_image(mm, main, pm, out.main, err);
    if (rc) return rc;

    // brk right after the main image when the host lets us, else wherever.
    {
        uint64_t want = lx::PageUp(out.main.highest);
        int64_t r = mm.map(want, params.brk_reserve, lx::prot_none, false, false, "[heap]");
        if (r < 0) {
            err = "cannot reserve brk";
            return (int)r;
        }
        out.brk_start = (uint64_t)r;
        out.brk_end = out.brk_start + params.brk_reserve;
        mm.brk_start = mm.brk_cur = out.brk_start;
        mm.brk_end = out.brk_end;
    }

    // Stack: guard page + RW range.
    {
        const uint64_t guard = AddressSpace::host_page();
        int64_t r = mm.map(0, params.stack_size + guard, lx::prot_none, false, false, "[stack]");
        if (r < 0) {
            err = "cannot map the stack";
            return (int)r;
        }
        out.stack_base = (uint64_t)r + guard;
        out.stack_top = out.stack_base + params.stack_size;
        int64_t pr = mm.protect(out.stack_base, params.stack_size, lx::prot_read | lx::prot_write);
        if (pr < 0) {
            err = "cannot protect the stack";
            return (int)pr;
        }
    }

    out.entry = out.has_interp ? out.interp.entry : out.main.entry;

    // ---- initial stack ----------------------------------------------------
    // Strings live at the top; then (going down) AT_RANDOM bytes, the auxv
    // table, envp[], argv[], argc. Final rsp is 16-byte aligned.
    std::string strings;
    std::vector<uint64_t> argv_off, envp_off;
    for (const auto& a : params.argv) {
        argv_off.push_back(strings.size());
        strings += a;
        strings.push_back('\0');
    }
    for (const auto& e : params.envp) {
        envp_off.push_back(strings.size());
        strings += e;
        strings.push_back('\0');
    }
    const uint64_t platform_off = strings.size();
    strings += "x86_64";
    strings.push_back('\0');
    const uint64_t execfn_off = strings.size();
    strings += params.exec_path.empty() ? params.argv[0] : params.exec_path;
    strings.push_back('\0');

    uint64_t sp = out.stack_top;
    sp -= strings.size();
    sp &= ~(uint64_t)15;
    const uint64_t strings_addr = sp;
    sp -= 16;  // AT_RANDOM
    const uint64_t random_addr = sp;

    std::vector<std::pair<uint64_t, uint64_t>> auxv = {
        {lx::at_sysinfo_ehdr, 0},  // patched out below (no vDSO): dropped
        {lx::at_minsigstksz, 3072},
        {lx::at_hwcap, params.hwcap},
        {lx::at_pagesz, lx::page},
        {lx::at_clktck, 100},
        {lx::at_phdr, out.main.phdr},
        {lx::at_phent, sizeof(lx::Elf64_Phdr)},
        {lx::at_phnum, out.main.phnum},
        {lx::at_base, out.has_interp ? out.interp.base : 0},
        {lx::at_flags, 0},
        {lx::at_entry, out.main.entry},
        {lx::at_uid, params.uid},
        {lx::at_euid, params.uid},
        {lx::at_gid, params.gid},
        {lx::at_egid, params.gid},
        {lx::at_secure, 0},
        {lx::at_random, random_addr},
        {lx::at_hwcap2, params.hwcap2},
        {lx::at_execfn, strings_addr + execfn_off},
        {lx::at_platform, strings_addr + platform_off},
        {lx::at_null, 0},
    };
    auxv.erase(auxv.begin());  // no vDSO

    const size_t nptr = 1 + params.argv.size() + 1 + params.envp.size() + 1 + 2 * auxv.size();
    sp -= nptr * 8;
    sp &= ~(uint64_t)15;
    out.rsp = sp;

    std::vector<uint64_t> block;
    block.reserve(nptr);
    block.push_back(params.argv.size());
    for (auto off : argv_off) block.push_back(strings_addr + off);
    block.push_back(0);
    for (auto off : envp_off) block.push_back(strings_addr + off);
    block.push_back(0);
    out.auxv = sp + block.size() * 8;
    out.auxv_entries = auxv.size();
    for (auto& [k, v] : auxv) {
        block.push_back(k);
        block.push_back(v);
    }
    out.at_random = random_addr;
    out.execfn = strings_addr + execfn_off;

    if (!mm.copy_in(sp, block.data(), block.size() * 8) || !mm.copy_in(random_addr, params.random.data(), 16) ||
        !mm.copy_in(strings_addr, strings.data(), strings.size())) {
        err = "cannot write the initial stack";
        return -lx::efault;
    }
    return 0;
}

std::string ElfLoader::LayoutJson(const ExecLayout& l) {
    char buf[1200];
    snprintf(buf, sizeof buf,
             "{\"entry\":\"0x%llx\",\"rsp\":\"0x%llx\",\"main\":{\"path\":\"%s\",\"dyn\":%s,\"base\":\"0x%llx\",\"entry\":\"0x%llx\","
             "\"range\":\"0x%llx-0x%llx\",\"phdr\":\"0x%llx\",\"phnum\":%u,\"interp\":\"%s\"},"
             "\"interp\":{\"base\":\"0x%llx\",\"entry\":\"0x%llx\",\"range\":\"0x%llx-0x%llx\"},"
             "\"stack\":\"0x%llx-0x%llx\",\"brk\":\"0x%llx-0x%llx\",\"auxv\":\"0x%llx\",\"auxv_entries\":%zu}",
             (unsigned long long)l.entry, (unsigned long long)l.rsp, JsonEscape(l.main.path).c_str(), l.main.is_dyn ? "true" : "false",
             (unsigned long long)l.main.base, (unsigned long long)l.main.entry, (unsigned long long)l.main.lowest,
             (unsigned long long)l.main.highest, (unsigned long long)l.main.phdr, (unsigned)l.main.phnum,
             JsonEscape(l.main.interp).c_str(), (unsigned long long)l.interp.base, (unsigned long long)l.interp.entry,
             (unsigned long long)l.interp.lowest, (unsigned long long)l.interp.highest, (unsigned long long)l.stack_base,
             (unsigned long long)l.stack_top, (unsigned long long)l.brk_start, (unsigned long long)l.brk_end,
             (unsigned long long)l.auxv, l.auxv_entries);
    return buf;
}

}  // namespace rlk
