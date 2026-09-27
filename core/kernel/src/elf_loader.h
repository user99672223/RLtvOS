// elf_loader.h — maps an x86-64 ELF (and its PT_INTERP) into a guest
// address space and builds the initial stack (argc/argv/envp/auxv) the way
// the Linux kernel does. Modelled on FEX's ELFCodeLoader, but reading
// through FileSource (network VFS) and mapping through AddressSpace.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "file_source.h"
#include "mm.h"

namespace rlk {

struct LoadedImage {
    std::string path;
    bool is_dyn = false;
    uint64_t base = 0;       // load bias (0 for ET_EXEC)
    uint64_t entry = 0;      // absolute
    uint64_t phdr = 0;       // absolute address of the program headers
    uint16_t phnum = 0;
    uint64_t lowest = 0, highest = 0;  // absolute mapped range
    std::string interp;      // PT_INTERP path ("" if none)
};

struct ExecParams {
    std::vector<std::string> argv;
    std::vector<std::string> envp;
    std::string exec_path;   // guest path of the executable (AT_EXECFN, /proc/self/exe)
    uint64_t hwcap = 0, hwcap2 = 0;
    uint32_t uid = 1000, gid = 1000;
    std::array<uint8_t, 16> random {};
    uint64_t stack_size = 8u << 20;
    uint64_t brk_reserve = 64u << 20;
};

struct ExecLayout {
    LoadedImage main, interp;
    bool has_interp = false;
    uint64_t entry = 0;      // where execution starts (interpreter entry if any)
    uint64_t stack_base = 0, stack_top = 0;  // [base, top) RW; guard page below base
    uint64_t rsp = 0;        // initial stack pointer (argc at *rsp)
    uint64_t brk_start = 0, brk_end = 0;
    uint64_t auxv = 0;       // address of the auxv table
    size_t auxv_entries = 0;
    uint64_t at_random = 0, execfn = 0;
};

// Opens another guest file (the interpreter) by guest path. nullptr = ENOENT.
using OpenFileFn = std::function<std::unique_ptr<FileSource>(const std::string& guest_path)>;

class ElfLoader {
public:
    // Parses the header only (validation + PT_INTERP). Returns 0 or -errno.
    static int Inspect(FileSource& file, LoadedImage& out, std::string& err);

    // Loads main (+ interpreter), reserves brk, maps the stack, writes the
    // initial stack. Returns 0 or -errno (Linux). On failure the mappings
    // already made stay in `mm` (the caller drops the address space).
    static int Load(AddressSpace& mm, FileSource& main, const ExecParams& params, const OpenFileFn& open,
                    ExecLayout& out, std::string& err);

    // JSON description of a layout for /run replies.
    static std::string LayoutJson(const ExecLayout& l);
};

}  // namespace rlk
