// linux_abi.h — the guest's ABI: Linux x86-64 constants and struct layouts.
// The host is Darwin (or Linux for the host tests), whose errno/flag values
// differ, so nothing in namespace lx may come from host headers.
//
// Names are the Linux spellings in lowercase (lx::enoent, lx::prot_read,
// lx::sigkill): the host headers and the C++ standard library define the
// uppercase spellings as macros and use them inline, so the uppercase names
// cannot coexist with them in one translation unit. Host calls made by the
// kernel itself (mmap, clock_gettime, open) use the host:: values below.
#pragma once

#include <cstddef>
#include <cstdint>

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>

// ---- host values the kernel needs for its own mmap/clock/open calls ---------
namespace host {
constexpr int kProtNone = PROT_NONE, kProtRead = PROT_READ, kProtWrite = PROT_WRITE, kProtExec = PROT_EXEC;
constexpr int kMapPrivate = MAP_PRIVATE, kMapAnon = MAP_ANONYMOUS, kMapFixed = MAP_FIXED;
constexpr int kORdonly = O_RDONLY, kOCloexec = O_CLOEXEC;
constexpr int kClockRealtime = CLOCK_REALTIME, kClockMonotonic = CLOCK_MONOTONIC,
              kClockProcessCputime = CLOCK_PROCESS_CPUTIME_ID, kClockThreadCputime = CLOCK_THREAD_CPUTIME_ID;
inline void* MapFailed() {
    return MAP_FAILED;
}

// Host errno → Linux errno (the guest's numbering). Darwin differs for most
// of the interesting ones (ELOOP 62/40, ENAMETOOLONG 63/36, EAGAIN 35/11).
inline int ErrnoToLinux(int e) {
    switch (e) {
        case 0: return 0;
        case EPERM: return 1;
        case ENOENT: return 2;
        case ESRCH: return 3;
        case EINTR: return 4;
        case EIO: return 5;
        case ENXIO: return 6;
        case E2BIG: return 7;
        case ENOEXEC: return 8;
        case EBADF: return 9;
        case ECHILD: return 10;
        case EAGAIN: return 11;
        case ENOMEM: return 12;
        case EACCES: return 13;
        case EFAULT: return 14;
        case EBUSY: return 16;
        case EEXIST: return 17;
        case EXDEV: return 18;
        case ENODEV: return 19;
        case ENOTDIR: return 20;
        case EISDIR: return 21;
        case EINVAL: return 22;
        case ENFILE: return 23;
        case EMFILE: return 24;
        case ENOTTY: return 25;
        case EFBIG: return 27;
        case ENOSPC: return 28;
        case ESPIPE: return 29;
        case EROFS: return 30;
        case EMLINK: return 31;
        case EPIPE: return 32;
        case ERANGE: return 34;
        case EDEADLK: return 35;
        case ENAMETOOLONG: return 36;
        case ENOSYS: return 38;
        case ENOTEMPTY: return 39;
        case ELOOP: return 40;
        case ENOTSOCK: return 88;
        case EOPNOTSUPP: return 95;
#if defined(ENOTSUP) && ENOTSUP != EOPNOTSUPP
        case ENOTSUP: return 95;
#endif
        case EAFNOSUPPORT: return 97;
        case ETIMEDOUT: return 110;
        case ECONNREFUSED: return 111;
        default: return 5;  // EIO
    }
}
}  // namespace host

namespace lx {

// ---- errno ---------------------------------------------------------------
constexpr int eperm = 1, enoent = 2, esrch = 3, eintr = 4, eio = 5, enxio = 6, e2big = 7, enoexec = 8, ebadf = 9,
              echild = 10, eagain = 11, enomem = 12, eacces = 13, efault = 14, ebusy = 16, eexist = 17, exdev = 18,
              enodev = 19, enotdir = 20, eisdir = 21, einval = 22, enfile = 23, emfile = 24, enotty = 25, efbig = 27,
              enospc = 28, espipe = 29, erofs = 30, emlink = 31, epipe = 32, erange = 34, edeadlk = 35,
              enametoolong = 36, enosys = 38, enotempty = 39, eloop = 40, enotsock = 88, eopnotsupp = 95,
              eafnosupport = 97, econnrefused = 111, etimedout = 110;

// ---- open / at ----------------------------------------------------------
constexpr int o_rdonly = 0, o_wronly = 1, o_rdwr = 2, o_accmode = 3, o_creat = 0100, o_excl = 0200, o_noctty = 0400,
              o_trunc = 01000, o_append = 02000, o_nonblock = 04000, o_dsync = 010000, o_directory = 0200000,
              o_nofollow = 0400000, o_cloexec = 02000000, o_path = 010000000, o_largefile = 0100000;
constexpr int at_fdcwd = -100, at_symlink_nofollow = 0x100, at_removedir = 0x200, at_symlink_follow = 0x400,
              at_empty_path = 0x1000;
constexpr int f_ok = 0, x_ok = 1, w_ok = 2, r_ok = 4;

// ---- mmap ----------------------------------------------------------------
constexpr int prot_none = 0, prot_read = 1, prot_write = 2, prot_exec = 4;
constexpr int map_shared = 0x01, map_private = 0x02, map_fixed = 0x10, map_anonymous = 0x20, map_growsdown = 0x100,
              map_denywrite = 0x800, map_noreserve = 0x4000, map_populate = 0x8000, map_stack = 0x20000,
              map_fixed_noreplace = 0x100000;
constexpr int madv_normal = 0, madv_random = 1, madv_sequential = 2, madv_willneed = 3, madv_dontneed = 4,
              madv_free = 8, madv_hugepage = 14, madv_nohugepage = 15, madv_dontdump = 16;

// ---- lseek / fcntl / ioctl ----------------------------------------------
constexpr int seek_set = 0, seek_cur = 1, seek_end = 2;
constexpr int f_dupfd = 0, f_getfd = 1, f_setfd = 2, f_getfl = 3, f_setfl = 4, f_getlk = 5, f_setlk = 6,
              f_setlkw = 7, f_dupfd_cloexec = 1030;
constexpr int fd_cloexec = 1;
constexpr unsigned tcgets = 0x5401, tcsets = 0x5402, tiocgpgrp = 0x540F, tiocgwinsz = 0x5413, fionread = 0x541B,
                   fioclex = 0x5451, fionbio = 0x5421;

// ---- arch_prctl / futex / clocks ----------------------------------------
constexpr int arch_set_gs = 0x1001, arch_set_fs = 0x1002, arch_get_fs = 0x1003, arch_get_gs = 0x1004;
constexpr int futex_wait = 0, futex_wake = 1, futex_requeue = 3, futex_cmp_requeue = 4, futex_wake_op = 5,
              futex_lock_pi = 6, futex_unlock_pi = 7, futex_wait_bitset = 9, futex_wake_bitset = 10,
              futex_private_flag = 128, futex_clock_realtime = 256, futex_cmd_mask = ~(128 | 256);
constexpr int clock_realtime = 0, clock_monotonic = 1, clock_process_cputime_id = 2, clock_thread_cputime_id = 3,
              clock_monotonic_raw = 4, clock_realtime_coarse = 5, clock_monotonic_coarse = 6, clock_boottime = 7;

// ---- rlimit ----------------------------------------------------------------
constexpr int rlimit_cpu = 0, rlimit_fsize = 1, rlimit_data = 2, rlimit_stack = 3, rlimit_core = 4, rlimit_rss = 5,
              rlimit_nproc = 6, rlimit_nofile = 7, rlimit_memlock = 8, rlimit_as = 9, rlimit_nlimits = 16;
constexpr uint64_t rlim_infinity = ~0ull;

// ---- signals ---------------------------------------------------------------
constexpr int sighup = 1, sigint = 2, sigquit = 3, sigill = 4, sigtrap = 5, sigabrt = 6, sigbus = 7, sigfpe = 8,
              sigkill = 9, sigusr1 = 10, sigsegv = 11, sigusr2 = 12, sigpipe = 13, sigalrm = 14, sigterm = 15,
              sigchld = 17, sigcont = 18, sigstop = 19, sigtstp = 20, sigsys = 31, nsig = 64;
constexpr int sig_block = 0, sig_unblock = 1, sig_setmask = 2;
constexpr uint64_t sa_restorer = 0x04000000;

// ---- auxv --------------------------------------------------------------------
constexpr uint64_t at_null = 0, at_phdr = 3, at_phent = 4, at_phnum = 5, at_pagesz = 6, at_base = 7, at_flags = 8,
                   at_entry = 9, at_uid = 11, at_euid = 12, at_gid = 13, at_egid = 14, at_platform = 15,
                   at_hwcap = 16, at_clktck = 17, at_secure = 23, at_random = 25, at_hwcap2 = 26, at_execfn = 31,
                   at_sysinfo_ehdr = 33, at_minsigstksz = 51;

// ---- file types --------------------------------------------------------------
constexpr uint32_t s_ifmt = 0170000, s_ifsock = 0140000, s_iflnk = 0120000, s_ifreg = 0100000, s_ifblk = 0060000,
                   s_ifdir = 0040000, s_ifchr = 0020000, s_ififo = 0010000;
constexpr uint8_t dt_unknown = 0, dt_fifo = 1, dt_chr = 2, dt_dir = 4, dt_blk = 6, dt_reg = 8, dt_lnk = 10,
                  dt_sock = 12;

// ---- struct layouts (x86-64) ----------------------------------------------
struct timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};
struct timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};
struct stat {  // 144 bytes
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t st_size;
    int64_t st_blksize;
    int64_t st_blocks;
    timespec st_atim;
    timespec st_mtim;
    timespec st_ctim;
    int64_t unused_[3];
};
static_assert(sizeof(stat) == 144, "linux stat layout");
struct iovec {
    uint64_t iov_base;
    uint64_t iov_len;
};
struct rlimit64 {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};
struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};
struct sigaction {  // kernel layout
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask;
};
struct dirent64_hdr {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    // char d_name[];  starts at byte 19 (the struct's sizeof is 24: tail padding)
};
constexpr size_t dirent64_hdr_size = 19;
struct sysinfo {
    int64_t uptime;
    uint64_t loads[3];
    uint64_t totalram, freeram, sharedram, bufferram, totalswap, freeswap;
    uint16_t procs;
    uint16_t pad;
    uint64_t totalhigh, freehigh;
    uint32_t mem_unit;
    char _f[20 - 2 * sizeof(uint64_t) - sizeof(uint32_t)];
};
struct winsize {
    uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel;
};

// ---- ELF64 ----------------------------------------------------------------------
constexpr uint16_t et_exec = 2, et_dyn = 3, em_x86_64 = 62;
constexpr uint32_t pt_load = 1, pt_dynamic = 2, pt_interp = 3, pt_note = 4, pt_phdr = 6, pt_tls = 7,
                   pt_gnu_stack = 0x6474e551;
constexpr uint32_t pf_x = 1, pf_w = 2, pf_r = 4;
struct Elf64_Ehdr {
    unsigned char e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};
struct Elf64_Phdr {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
};
static_assert(sizeof(Elf64_Ehdr) == 64 && sizeof(Elf64_Phdr) == 56, "elf64 layout");

constexpr uint64_t page = 4096;
constexpr uint64_t page_mask = ~(page - 1);
inline uint64_t PageDown(uint64_t x) { return x & page_mask; }
inline uint64_t PageUp(uint64_t x) { return (x + page - 1) & page_mask; }

}  // namespace lx
