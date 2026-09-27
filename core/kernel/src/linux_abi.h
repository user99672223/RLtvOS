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
              enametoolong = 36, enolck = 37, enosys = 38, enotempty = 39, eloop = 40, eoverflow = 75, enotsock = 88,
              edestaddrreq = 89, emsgsize = 90, eprototype = 91, enoprotoopt = 92, eprotonosupport = 93,
              esocktnosupport = 94, eopnotsupp = 95, eafnosupport = 97, eaddrinuse = 98, eaddrnotavail = 99,
              enetunreach = 101, econnreset = 104, enobufs = 105, eisconn = 106, enotconn = 107, eshutdown = 108,
              etoomanyrefs = 109, etimedout = 110, econnrefused = 111, ealready = 114, einprogress = 115;

// ---- open / at ----------------------------------------------------------
constexpr int o_rdonly = 0, o_wronly = 1, o_rdwr = 2, o_accmode = 3, o_creat = 0100, o_excl = 0200, o_noctty = 0400,
              o_trunc = 01000, o_append = 02000, o_nonblock = 04000, o_dsync = 010000, o_directory = 0200000,
              o_nofollow = 0400000, o_noatime = 01000000, o_cloexec = 02000000, o_path = 010000000,
              o_tmpfile = 020000000 | 0200000, o_largefile = 0100000;
constexpr int at_fdcwd = -100, at_symlink_nofollow = 0x100, at_removedir = 0x200, at_symlink_follow = 0x400,
              at_empty_path = 0x1000;
constexpr int f_ok = 0, x_ok = 1, w_ok = 2, r_ok = 4;
constexpr unsigned rename_noreplace = 1, rename_exchange = 2, rename_whiteout = 4;
constexpr int64_t utime_now = (1 << 30) - 1, utime_omit = (1 << 30) - 2;
constexpr unsigned mfd_cloexec = 1, mfd_allow_sealing = 2;
constexpr uint64_t tmpfs_magic = 0x01021994, proc_super_magic = 0x9fa0, ext4_super_magic = 0xEF53,
                   sockfs_magic = 0x534F434B, pipefs_magic = 0x50495045;

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
              f_setlkw = 7, f_setown = 8, f_getown = 9, f_setsig = 10, f_getsig = 11, f_ofd_getlk = 36,
              f_ofd_setlk = 37, f_ofd_setlkw = 38, f_dupfd_cloexec = 1030, f_setpipe_sz = 1031, f_getpipe_sz = 1032,
              f_add_seals = 1033, f_get_seals = 1034;
constexpr int f_rdlck = 0, f_wrlck = 1, f_unlck = 2;
constexpr int lock_sh = 1, lock_ex = 2, lock_nb = 4, lock_un = 8;
constexpr int fd_cloexec = 1;
struct flock {  // 32 bytes
    int16_t l_type;
    int16_t l_whence;
    int32_t pad0;
    int64_t l_start;
    int64_t l_len;
    int32_t l_pid;
    int32_t pad1;
};
static_assert(sizeof(flock) == 32, "linux flock layout");
constexpr unsigned tcgets = 0x5401, tcsets = 0x5402, tiocgpgrp = 0x540F, tiocspgrp = 0x5410, tiocgwinsz = 0x5413,
                   fionread = 0x541B, fioclex = 0x5451, fionclex = 0x5450, fionbio = 0x5421, siocgifconf = 0x8912,
                   siocgifindex = 0x8933, siocgifflags = 0x8913;

// ---- poll / select / epoll ---------------------------------------------------
constexpr unsigned pollin = 1, pollpri = 2, pollout = 4, pollerr = 8, pollhup = 0x10, pollnval = 0x20, pollrdnorm = 0x40,
                   pollrdband = 0x80, pollwrnorm = 0x100, pollwrband = 0x200, pollmsg = 0x400, pollrdhup = 0x2000;
struct pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};
constexpr int fd_setsize = 1024;
constexpr int epoll_cloexec = 02000000;
constexpr int epoll_ctl_add = 1, epoll_ctl_del = 2, epoll_ctl_mod = 3;
constexpr uint32_t epollin = 1, epollpri = 2, epollout = 4, epollerr = 8, epollhup = 0x10, epollrdnorm = 0x40,
                   epollrdband = 0x80, epollwrnorm = 0x100, epollwrband = 0x200, epollmsg = 0x400, epollrdhup = 0x2000,
                   epollexclusive = 1u << 28, epollwakeup = 1u << 29, epolloneshot = 1u << 30, epollet = 1u << 31;
struct __attribute__((packed)) epoll_event {  // x86-64: 12 bytes, packed
    uint32_t events;
    uint64_t data;
};
static_assert(sizeof(epoll_event) == 12, "linux epoll_event layout");
constexpr int efd_semaphore = 1, efd_cloexec = 02000000, efd_nonblock = 04000;
constexpr int tfd_cloexec = 02000000, tfd_nonblock = 04000, tfd_timer_abstime = 1;

// ---- sockets ------------------------------------------------------------------
constexpr int af_unspec = 0, af_unix = 1, af_inet = 2, af_inet6 = 10, af_netlink = 16;
constexpr int sock_stream = 1, sock_dgram = 2, sock_raw = 3, sock_seqpacket = 5, sock_type_mask = 0xf,
              sock_nonblock = 04000, sock_cloexec = 02000000;
constexpr int sol_socket = 1;
constexpr int so_debug = 1, so_reuseaddr = 2, so_type = 3, so_error = 4, so_dontroute = 5, so_broadcast = 6,
              so_sndbuf = 7, so_rcvbuf = 8, so_keepalive = 9, so_oobinline = 10, so_linger = 13, so_reuseport = 15,
              so_passcred = 16, so_peercred = 17, so_rcvlowat = 18, so_sndlowat = 19, so_rcvtimeo = 20,
              so_sndtimeo = 21, so_acceptconn = 30, so_protocol = 38, so_domain = 39;
constexpr int scm_rights = 1, scm_credentials = 2;
constexpr int msg_oob = 1, msg_peek = 2, msg_dontroute = 4, msg_ctrunc = 8, msg_trunc = 0x20, msg_dontwait = 0x40,
              msg_eor = 0x80, msg_waitall = 0x100, msg_nosignal = 0x4000, msg_cmsg_cloexec = 0x40000000;
constexpr int shut_rd = 0, shut_wr = 1, shut_rdwr = 2;
struct sockaddr_un {
    uint16_t sun_family;
    char sun_path[108];
};
struct msghdr {  // 56 bytes
    uint64_t msg_name;
    uint32_t msg_namelen;
    uint32_t pad0;
    uint64_t msg_iov;
    uint64_t msg_iovlen;
    uint64_t msg_control;
    uint64_t msg_controllen;
    uint32_t msg_flags;
    uint32_t pad1;
};
static_assert(sizeof(msghdr) == 56, "linux msghdr layout");
struct cmsghdr {  // followed by data, padded to 8
    uint64_t cmsg_len;
    int32_t cmsg_level;
    int32_t cmsg_type;
};
inline uint64_t CmsgAlign(uint64_t n) { return (n + 7) & ~7ull; }
struct ucred {
    int32_t pid, uid, gid;
};

// ---- itimers (struct itimerval follows struct timeval below) --------------------------
constexpr int itimer_real = 0, itimer_virtual = 1, itimer_prof = 2;

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
                   s_ifdir = 0040000, s_ifchr = 0020000, s_ififo = 0010000, s_isuid = 04000, s_isgid = 02000,
                   s_isvtx = 01000;
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
struct itimerval {
    timeval it_interval;
    timeval it_value;
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

// ---- signals: actions, flags, codes --------------------------------------------
constexpr uint64_t sig_dfl = 0, sig_ign = 1;
constexpr uint64_t sa_nocldstop = 1, sa_nocldwait = 2, sa_siginfo = 4, sa_onstack = 0x08000000, sa_restart = 0x10000000,
                   sa_nodefer = 0x40000000, sa_resethand = 0x80000000;
constexpr int si_user = 0, si_kernel = 0x80, si_queue = -1, si_tkill = -6;
constexpr int cld_exited = 1, cld_killed = 2, cld_dumped = 3;
constexpr int segv_maperr = 1, segv_accerr = 2, bus_adraln = 1, ill_illopc = 1, fpe_intdiv = 1;
constexpr int ss_onstack = 1, ss_disable = 2;
constexpr uint64_t minsigstksz = 2048;
constexpr uint64_t sigbit(int sig) { return 1ull << (sig - 1); }

// ---- clone / wait ----------------------------------------------------------------
constexpr uint64_t clone_vm = 0x100, clone_fs = 0x200, clone_files = 0x400, clone_sighand = 0x800, clone_vfork = 0x4000,
                   clone_parent = 0x8000, clone_thread = 0x10000, clone_settls = 0x80000, clone_parent_settid = 0x100000,
                   clone_child_cleartid = 0x200000, clone_child_settid = 0x1000000, clone_csignal = 0xff;
constexpr int wnohang = 1, wuntraced = 2, wexited = 4, wcontinued = 8, wnowait = 0x1000000;
inline int wstatus_exited(int code) { return (code & 0xff) << 8; }
inline int wstatus_signaled(int sig) { return sig & 0x7f; }

// ---- signal frame layouts (x86-64, kernel's view) --------------------------------
struct stack_t {  // 24 bytes
    uint64_t ss_sp;
    int32_t ss_flags;
    int32_t pad;
    uint64_t ss_size;
};
struct siginfo {  // 128 bytes
    int32_t si_signo, si_errno, si_code, pad0;
    union {
        struct {
            int32_t pid, uid;
        } kill;
        struct {
            int32_t pid, uid, status, pad;
            int64_t utime, stime;
        } chld;
        struct {
            uint64_t addr;
        } fault;
        uint8_t raw[112];
    } u;
};
struct sigcontext {  // 256 bytes
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags;
    uint16_t cs, gs, fs, ss;
    uint64_t err, trapno, oldmask, cr2;
    uint64_t fpstate;  // pointer to the 512-byte fxsave area
    uint64_t reserved[8];
};
struct ucontext {  // 304 bytes (the kernel's ucontext: 8-byte sigset)
    uint64_t uc_flags, uc_link;
    stack_t uc_stack;
    sigcontext uc_mcontext;
    uint64_t uc_sigmask;
};
struct rt_sigframe {  // 440 bytes; the fxsave area (512, 64-aligned) sits above it
    uint64_t pretcode;  // the handler returns here: sa_restorer (rt_sigreturn)
    ucontext uc;
    siginfo info;
};
static_assert(sizeof(siginfo) == 128 && sizeof(sigcontext) == 256 && sizeof(ucontext) == 304 && sizeof(rt_sigframe) == 440,
              "x86-64 signal frame layout");
constexpr size_t fxsave_size = 512;

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
