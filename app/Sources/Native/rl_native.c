// rl_native.c — JIT/VA/memory probes, sysctl info, crash handler.
#include "rl_native.h"
#include "rl_log.h"

#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <libkern/OSCacheControl.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/task_info.h>
#include <os/proc.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/ucontext.h>
#include <unistd.h>

// ---------------------------------------------------------------- debug flags

extern int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
#define RL_CS_OPS_STATUS 0
#define RL_CS_DEBUGGED 0x10000000u

int rl_is_ptraced(void) {
    struct kinfo_proc info;
    size_t size = sizeof(info);
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    memset(&info, 0, sizeof info);
    if (sysctl(mib, 4, &info, &size, NULL, 0) != 0) return -1;
    return (info.kp_proc.p_flag & P_TRACED) != 0;
}

int rl_cs_debugged(void) {
    uint32_t flags = 0;
    if (csops(getpid(), RL_CS_OPS_STATUS, &flags, sizeof flags) != 0) return -1;
    return (flags & RL_CS_DEBUGGED) != 0;
}

// ---------------------------------------------------------------- JIT test

// mov w0,#42 ; ret
static const uint32_t k_code42[2] = {0x52800540u, 0xD65F03C0u};
// mov w0,#43 ; ret
static const uint32_t k_code43[2] = {0x52800560u, 0xD65F03C0u};

int rl_exec_words(const uint32_t *code_words, size_t n_words, int *err) {
    size_t sz = (n_words * 4 + 16383) & ~(size_t)16383;
    void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { if (err) *err = errno; return 0; }
    memcpy(p, code_words, n_words * 4);
    sys_icache_invalidate(p, n_words * 4);
    int (*fn)(void) = (int (*)(void))p;
    int r = fn();
    munmap(p, sz);
    if (err) *err = 0;
    return r;
}

int rl_jit_test_json(int execute, char *out, size_t cap) {
    const size_t sz = 16384;
    int ptraced = rl_is_ptraced();
    int csd = rl_cs_debugged();
    const char *method = "rwx-mmap";
    int rwx_errno = 0;

    void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        rwx_errno = errno;
        p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) {
            snprintf(out, cap,
                     "{\"ok\":false,\"stage\":\"mmap\",\"errno\":%d,\"rwx_errno\":%d,"
                     "\"ptraced\":%d,\"cs_debugged\":%d}",
                     errno, rwx_errno, ptraced, csd);
            return 0;
        }
        method = "rw-then-rx";
    }

    memcpy(p, k_code42, sizeof k_code42);
    if (strcmp(method, "rw-then-rx") == 0) {
        if (mprotect(p, sz, PROT_READ | PROT_EXEC) != 0) {
            int e = errno;
            munmap(p, sz);
            snprintf(out, cap,
                     "{\"ok\":false,\"stage\":\"mprotect-rx\",\"errno\":%d,\"rwx_errno\":%d,"
                     "\"ptraced\":%d,\"cs_debugged\":%d}",
                     e, rwx_errno, ptraced, csd);
            return 0;
        }
    }
    sys_icache_invalidate(p, sizeof k_code42);

    if (!execute) {
        munmap(p, sz);
        snprintf(out, cap,
                 "{\"ok\":false,\"stage\":\"mapped-only\",\"method\":\"%s\",\"rwx_errno\":%d,"
                 "\"ptraced\":%d,\"cs_debugged\":%d,\"reason\":\"not executed: on tvOS 26+ a page "
                 "runs only after a debugger wrote to it; use tv.py jit (authorizes the arena, then "
                 "runs jittest --trust)\"}",
                 method, rwx_errno, ptraced, csd);
        return 0;
    }

    int (*fn)(void) = (int (*)(void))p;
    int r1 = fn();

    // Write-after-execute: a JIT patches code it already ran.
    int r2 = -1;
    int rewrite_errno = 0;
    if (strcmp(method, "rw-then-rx") == 0) {
        if (mprotect(p, sz, PROT_READ | PROT_WRITE) != 0) rewrite_errno = errno;
    }
    if (rewrite_errno == 0) {
        memcpy(p, k_code43, sizeof k_code43);
        if (strcmp(method, "rw-then-rx") == 0) {
            if (mprotect(p, sz, PROT_READ | PROT_EXEC) != 0) rewrite_errno = errno;
        }
        if (rewrite_errno == 0) {
            sys_icache_invalidate(p, sizeof k_code43);
            r2 = fn();
        }
    }
    munmap(p, sz);

    int ok = (r1 == 42 && r2 == 43);
    snprintf(out, cap,
             "{\"ok\":%s,\"method\":\"%s\",\"result1\":%d,\"result2\":%d,\"rwx_errno\":%d,"
             "\"rewrite_errno\":%d,\"ptraced\":%d,\"cs_debugged\":%d,\"page\":%zu}",
             ok ? "true" : "false", method, r1, r2, rwx_errno, rewrite_errno, ptraced, csd, sz);
    return ok;
}

// ---------------------------------------------------------------- JIT arena

static void *g_arena_base;
static size_t g_arena_size;
static int g_arena_rwx;
static int g_arena_rwx_errno;

int rl_jit_arena_init(size_t size) {
    if (g_arena_base) return -EEXIST;
    size = (size + 16383) & ~(size_t)16383;
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        g_arena_rwx_errno = errno;
        p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) return -errno;
        g_arena_rwx = 0;
    } else {
        g_arena_rwx = 1;
    }
    g_arena_base = p;
    g_arena_size = size;
    return 0;
}

void rl_jit_arena_json(char *out, size_t cap) {
    snprintf(out, cap,
             "{\"base\":\"0x%llx\",\"size\":%zu,\"size_mb\":%zu,\"prot\":\"%s\",\"rwx_errno\":%d,"
             "\"page\":16384,\"pages\":%zu}",
             (unsigned long long)(uintptr_t)g_arena_base, g_arena_size, g_arena_size >> 20,
             g_arena_base ? (g_arena_rwx ? "rwx" : "rw") : "none", g_arena_rwx_errno,
             g_arena_size / 16384);
}

int rl_jit_exec_test_json(int page, int madv, int fresh, char *out, size_t cap) {
    const size_t PG = 16384;
    void *p;
    int rwx;
    const char *where;
    int fresh_errno = 0;
    if (fresh) {
        p = mmap(NULL, PG, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
        rwx = 1;
        if (p == MAP_FAILED) {
            fresh_errno = errno;
            p = mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
            rwx = 0;
            if (p == MAP_FAILED) {
                snprintf(out, cap, "{\"ok\":false,\"stage\":\"mmap\",\"errno\":%d,\"rwx_errno\":%d}", errno, fresh_errno);
                return 0;
            }
        }
        where = "fresh";
    } else {
        if (!g_arena_base) { snprintf(out, cap, "{\"ok\":false,\"error\":\"arena not initialised\"}"); return 0; }
        if (page < 0 || (size_t)page * PG >= g_arena_size) {
            snprintf(out, cap, "{\"ok\":false,\"error\":\"page out of range\",\"pages\":%zu}", g_arena_size / PG);
            return 0;
        }
        p = (char *)g_arena_base + (size_t)page * PG;
        rwx = g_arena_rwx;
        where = "arena";
    }
    int madv_rc = 0, madv_errno = 0;
    if (madv) {
        madv_rc = madvise(p, PG, MADV_FREE);
        if (madv_rc != 0) madv_errno = errno;
    }
    int (*fn)(void) = (int (*)(void))p;
    int prot_errno = 0;
    int r1 = -1, r2 = -1;

    if (!rwx && mprotect(p, PG, PROT_READ | PROT_WRITE) != 0) prot_errno = errno;
    memcpy(p, k_code42, sizeof k_code42);
    if (!rwx && prot_errno == 0 && mprotect(p, PG, PROT_READ | PROT_EXEC) != 0) prot_errno = errno;
    if (prot_errno == 0) {
        sys_icache_invalidate(p, sizeof k_code42);
        r1 = fn();
        if (!rwx && mprotect(p, PG, PROT_READ | PROT_WRITE) != 0) prot_errno = errno;
        if (prot_errno == 0) {
            memcpy(p, k_code43, sizeof k_code43);
            if (!rwx && mprotect(p, PG, PROT_READ | PROT_EXEC) != 0) prot_errno = errno;
            if (prot_errno == 0) {
                sys_icache_invalidate(p, sizeof k_code43);
                r2 = fn();
            }
        }
    }
    if (fresh) munmap(p, PG);
    int ok = (r1 == 42 && r2 == 43);
    snprintf(out, cap,
             "{\"ok\":%s,\"where\":\"%s\",\"page\":%d,\"addr\":\"0x%llx\",\"prot\":\"%s\",\"result1\":%d,"
             "\"result2\":%d,\"prot_errno\":%d,\"fresh_rwx_errno\":%d,\"madvise\":%d,\"madvise_rc\":%d,"
             "\"madvise_errno\":%d,\"ptraced\":%d,\"cs_debugged\":%d}",
             ok ? "true" : "false", where, fresh ? -1 : page, (unsigned long long)(uintptr_t)p,
             rwx ? "rwx" : "rw/rx", r1, r2, prot_errno, fresh_errno, madv, madv_rc, madv_errno,
             rl_is_ptraced(), rl_cs_debugged());
    return ok;
}

// ---------------------------------------------------------------- VA probe

void rl_va_probe_json(char *out, size_t cap, int step_limit_gb) {
    const size_t GB = (size_t)1 << 30;
    const int flags = MAP_PRIVATE | MAP_ANON | MAP_NORESERVE;
    void *p;

    // Largest single reservation: double until failure, then binary search.
    size_t lo = 0, hi = 0, try = 1;
    while (try <= 65536) {
        p = mmap(NULL, try * GB, PROT_NONE, flags, -1, 0);
        if (p == MAP_FAILED) break;
        munmap(p, try * GB);
        lo = try;
        try *= 2;
    }
    hi = try;
    int contig_errno = errno;
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        p = mmap(NULL, mid * GB, PROT_NONE, flags, -1, 0);
        if (p == MAP_FAILED) hi = mid;
        else { munmap(p, mid * GB); lo = mid; }
    }
    size_t max_contig = lo;

    // Total: 1 GB steps back to back.
    if (step_limit_gb <= 0) step_limit_gb = 1024;
    void **chunks = calloc((size_t)step_limit_gb, sizeof(void *));
    int n = 0;
    int stop_errno = 0;
    uintptr_t lowest = UINTPTR_MAX, highest = 0;
    if (chunks) {
        for (; n < step_limit_gb; n++) {
            p = mmap(NULL, GB, PROT_NONE, flags, -1, 0);
            if (p == MAP_FAILED) { stop_errno = errno; break; }
            chunks[n] = p;
            if ((uintptr_t)p < lowest) lowest = (uintptr_t)p;
            if ((uintptr_t)p + GB > highest) highest = (uintptr_t)p + GB;
        }
        for (int i = 0; i < n; i++) munmap(chunks[i], GB);
        free(chunks);
    }
    if (n == 0) lowest = 0;

    snprintf(out, cap,
             "{\"max_contiguous_gb\":%zu,\"contig_errno\":%d,\"total_1gb_steps\":%d,"
             "\"step_limit_gb\":%d,\"stop_errno\":%d,\"lowest\":\"0x%llx\",\"highest\":\"0x%llx\"}",
             max_contig, contig_errno, n, step_limit_gb, stop_errno,
             (unsigned long long)lowest, (unsigned long long)highest);
}

// ---------------------------------------------------------------- memory

static uint64_t g_peak_footprint;

void rl_mem_json(char *out, size_t cap) {
    task_vm_info_data_t vm;
    mach_msg_type_number_t cnt = TASK_VM_INFO_COUNT;
    memset(&vm, 0, sizeof vm);
    kern_return_t kr = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm, &cnt);
    size_t avail = os_proc_available_memory();
    if (kr == KERN_SUCCESS && vm.phys_footprint > g_peak_footprint) g_peak_footprint = vm.phys_footprint;
    snprintf(out, cap,
             "{\"kr\":%d,\"phys_footprint\":%llu,\"peak_phys_footprint\":%llu,\"resident_size\":%llu,"
             "\"virtual_size\":%llu,\"internal\":%llu,\"compressed\":%llu,\"external\":%llu,"
             "\"available\":%zu,\"limit_estimate\":%llu,\"phys_footprint_mb\":%.1f,\"available_mb\":%.1f}",
             (int)kr, (unsigned long long)vm.phys_footprint, (unsigned long long)g_peak_footprint,
             (unsigned long long)vm.resident_size, (unsigned long long)vm.virtual_size,
             (unsigned long long)vm.internal, (unsigned long long)vm.compressed,
             (unsigned long long)vm.external, avail,
             (unsigned long long)(vm.phys_footprint + avail),
             (double)vm.phys_footprint / (1024.0 * 1024.0), (double)avail / (1024.0 * 1024.0));
}

// ---------------------------------------------------------------- sysinfo

static void sysctl_str(const char *name, char *buf, size_t cap) {
    size_t len = cap;
    if (sysctlbyname(name, buf, &len, NULL, 0) != 0 || len == 0) { snprintf(buf, cap, "?"); return; }
    buf[len < cap ? len : cap - 1] = 0;
}

static uint64_t sysctl_u64(const char *name) {
    uint64_t v = 0;
    size_t len = sizeof v;
    if (sysctlbyname(name, &v, &len, NULL, 0) != 0) return 0;
    if (len == 4) v = *(uint32_t *)&v;
    return v;
}

void rl_sysinfo_json(char *out, size_t cap) {
    char machine[64], osver[64], model[64];
    sysctl_str("hw.machine", machine, sizeof machine);
    sysctl_str("kern.osversion", osver, sizeof osver);
    sysctl_str("hw.model", model, sizeof model);
    snprintf(out, cap,
             "{\"hw_machine\":\"%s\",\"hw_model\":\"%s\",\"kern_osversion\":\"%s\",\"hw_memsize\":%llu,"
             "\"hw_pagesize\":%llu,\"ncpu\":%llu,\"pid\":%d,\"ptraced\":%d,\"cs_debugged\":%d,"
             "\"sigaltstack\":\"%s\"}",
             machine, model, osver, (unsigned long long)sysctl_u64("hw.memsize"),
             (unsigned long long)sysctl_u64("hw.pagesize"), (unsigned long long)sysctl_u64("hw.ncpu"),
             (int)getpid(), rl_is_ptraced(), rl_cs_debugged(), rl_altstack_status());
}

// ---------------------------------------------------------------- crash handler

static char g_crash_path[1024];
static stack_t g_altstack;
static char g_altstack_status[64] = "not-tried";
static int g_altstack_ok;

// sigaltstack is __TVOS_PROHIBITED in the SDK headers (compile-time only).
// The syscall exists in XNU; resolve the libsystem symbol at runtime and
// record whether it works on this box — FEX's signal delegator wants it.
typedef int (*rl_sigaltstack_fn)(const stack_t *, stack_t *);

static int install_altstack(void) {
    if (g_altstack_ok) return 0;
    rl_sigaltstack_fn fn = (rl_sigaltstack_fn)dlsym(RTLD_DEFAULT, "sigaltstack");
    if (!fn) {
        snprintf(g_altstack_status, sizeof g_altstack_status, "symbol-missing");
        return -1;
    }
    if (!g_altstack.ss_sp) {
        g_altstack.ss_size = 256 * 1024;
        g_altstack.ss_sp = malloc(g_altstack.ss_size);
        g_altstack.ss_flags = 0;
    }
    if (!g_altstack.ss_sp) { snprintf(g_altstack_status, sizeof g_altstack_status, "oom"); return -1; }
    if (fn(&g_altstack, NULL) != 0) {
        snprintf(g_altstack_status, sizeof g_altstack_status, "errno=%d", errno);
        return -1;
    }
    stack_t cur;
    memset(&cur, 0, sizeof cur);
    if (fn(NULL, &cur) == 0 && cur.ss_sp == g_altstack.ss_sp)
        snprintf(g_altstack_status, sizeof g_altstack_status, "ok");
    else
        snprintf(g_altstack_status, sizeof g_altstack_status, "set-but-readback-differs");
    g_altstack_ok = 1;
    return 0;
}

const char *rl_altstack_status(void) { return g_altstack_status; }

static const char *signame(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS: return "SIGBUS";
        case SIGILL: return "SIGILL";
        case SIGFPE: return "SIGFPE";
        case SIGTRAP: return "SIGTRAP";
        case SIGABRT: return "SIGABRT";
        default: return "SIG?";
    }
}

static void write_all(int fd, const char *s, size_t n) {
    while (n > 0) {
        ssize_t w = write(fd, s, n);
        if (w <= 0) return;
        s += w;
        n -= (size_t)w;
    }
}

static void crash_handler(int sig, siginfo_t *si, void *uctx) {
    char buf[1024];
    uint64_t pc = 0, lr = 0, sp = 0, fp = 0, far = 0, esr = 0;
    ucontext_t *uc = (ucontext_t *)uctx;
#if defined(__arm64__) || defined(__aarch64__)
    if (uc && uc->uc_mcontext) {
        pc = __darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
        lr = __darwin_arm_thread_state64_get_lr(uc->uc_mcontext->__ss);
        sp = __darwin_arm_thread_state64_get_sp(uc->uc_mcontext->__ss);
        fp = __darwin_arm_thread_state64_get_fp(uc->uc_mcontext->__ss);
        far = uc->uc_mcontext->__es.__far;
        esr = uc->uc_mcontext->__es.__esr;
    }
#endif
    intptr_t slide = _dyld_get_image_vmaddr_slide(0);
    int fd = open(g_crash_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int n = snprintf(buf, sizeof buf,
                     "crash: %s (%d) code=%d addr=%p\n"
                     "pc=0x%llx lr=0x%llx sp=0x%llx fp=0x%llx far=0x%llx esr=0x%llx\n"
                     "slide=0x%lx unslid_pc=0x%llx unslid_lr=0x%llx image=%s\n"
                     "uptime_ms=%.0f\nbacktrace:\n",
                     signame(sig), sig, si ? si->si_code : 0, si ? si->si_addr : NULL,
                     (unsigned long long)pc, (unsigned long long)lr, (unsigned long long)sp,
                     (unsigned long long)fp, (unsigned long long)far, (unsigned long long)esr,
                     (long)slide, (unsigned long long)(pc - (uint64_t)slide),
                     (unsigned long long)(lr - (uint64_t)slide), _dyld_get_image_name(0),
                     rl_uptime_ms());
    if (n < 0) n = 0;
    if (fd >= 0) write_all(fd, buf, (size_t)n);
    write_all(2, buf, (size_t)n);

    void *frames[96];
    int cnt = backtrace(frames, 96);
    if (fd >= 0) backtrace_symbols_fd(frames, cnt, fd);
    backtrace_symbols_fd(frames, cnt, 2);

    // Raw frame addresses (unslid) for offline symbolication with the dSYM.
    n = snprintf(buf, sizeof buf, "frames_unslid:");
    for (int i = 0; i < cnt && n < (int)sizeof buf - 24; i++)
        n += snprintf(buf + n, sizeof buf - (size_t)n, " 0x%llx",
                      (unsigned long long)((uintptr_t)frames[i] - (uintptr_t)slide));
    n += snprintf(buf + n, sizeof buf - (size_t)n, "\n");
    if (fd >= 0) { write_all(fd, buf, (size_t)n); close(fd); }

    // Restore default disposition and let the fault re-deliver (synchronous
    // faults) or re-raise (abort).
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
    if (sig == SIGABRT || sig == SIGTRAP) raise(sig);
}

int rl_crash_install(const char *path) {
    snprintf(g_crash_path, sizeof g_crash_path, "%s", path ? path : "/dev/null");
    install_altstack();
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | (g_altstack_ok ? SA_ONSTACK : 0);
    sigemptyset(&sa.sa_mask);
    const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT};
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
        if (sigaction(sigs[i], &sa, NULL) != 0) return -errno;
    signal(SIGPIPE, SIG_IGN);
    return 0;
}

void rl_crash_now(int kind) {
    rl_logf("host: rl_crash_now(%d)", kind);
    if (kind == 1) abort();
    if (kind == 2) __builtin_trap();
    volatile int *p = (volatile int *)(uintptr_t)0x10;
    *p = 1;
    abort();
}
