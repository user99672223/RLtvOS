// jit26.c — TXM JIT pool: brk #0xf00d stubs, debugger-prepared RX region,
// vm_remap'ed RW alias, self-tests, first-fit sub-allocator, SIGTRAP guard.
// See jit26.h and handoff/issues/001-jit.md.
#include "jit26.h"
#include "rl_log.h"
#include "rl_native.h"

#include <errno.h>
#include <fcntl.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/vm_map.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/ucontext.h>
#include <unistd.h>

#define PG 16384u
#define MAX_SEGS 1024

// ---------------------------------------------------------------- stubs
//
// Must live in the main executable (the debugger script checks the image)
// and must be exactly `mov x16, #N ; brk #0xf00d ; ret`. x16 selects the
// operation, x0/x1 carry the arguments, x0 carries the result.
#if defined(__arm64__) || defined(__aarch64__)
__attribute__((naked, noinline, used, visibility("default"))) void *JIT26PrepareRegion(void *addr, size_t len) {
    __asm__ volatile(
        "mov x16, #1\n"
        "brk #0xf00d\n"
        "ret\n");
}

__attribute__((naked, noinline, used, visibility("default"))) void JIT26Detach(void) {
    __asm__ volatile(
        "mov x16, #0\n"
        "brk #0xf00d\n"
        "ret\n");
}
#else
void *JIT26PrepareRegion(void *addr, size_t len) { (void)addr; (void)len; return NULL; }
void JIT26Detach(void) {}
#endif

// ---------------------------------------------------------------- state

struct seg { size_t off, len; int used; };

static struct {
    pthread_mutex_t lock;      // state + allocator
    pthread_mutex_t brk_lock;  // serialises the two stubs
    uint8_t *rx, *rw;
    size_t size;
    int ready, in_progress, started;
    char stage[32];
    char err[200];
    const char *prepared_by;
    int in_place_tried, in_place_errno, in_place_prepared;
    int attached_test, detached_test; // -1 = not run, 0 = failed, 1 = ok
    int detached, detach_rc;
    int wait_s_asked;
    double waited_s, prepare_s, remap_s;
    int ptraced_at_prepare;
    int unserviced_traps;
    kern_return_t remap_kr, protect_kr;
    struct seg segs[MAX_SEGS];
    int nsegs;
    char marker[1024];
} g = {.lock = PTHREAD_MUTEX_INITIALIZER, .brk_lock = PTHREAD_MUTEX_INITIALIZER,
       .stage = "idle", .prepared_by = "none", .attached_test = -1, .detached_test = -1};

static double now_s(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

static void set_stage(const char *s) {
    pthread_mutex_lock(&g.lock);
    snprintf(g.stage, sizeof g.stage, "%s", s);
    pthread_mutex_unlock(&g.lock);
    rl_logf("jit26: stage %s", s);
}

static void set_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&g.lock);
    vsnprintf(g.err, sizeof g.err, fmt, ap);
    pthread_mutex_unlock(&g.lock);
    va_end(ap);
    rl_logf("jit26: error: %s", g.err);
}

void rl_jit26_set_marker_path(const char *path) {
    snprintf(g.marker, sizeof g.marker, "%s", path ? path : "");
}

static void write_marker(const char *what) {
    if (!g.marker[0]) return;
    int fd = open(g.marker, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    char buf[256];
    int n = snprintf(buf, sizeof buf, "jit26 %s prepared_by=%s rx=%p rw=%p size=%zu\n", what, g.prepared_by,
                     (void *)g.rx, (void *)g.rw, g.size);
    if (n > 0) (void)!write(fd, buf, (size_t)n);
    close(fd);
}

static void clear_marker(void) {
    if (g.marker[0]) unlink(g.marker);
}

// ---------------------------------------------------------------- self-test

// mov w0,#imm ; ret
static uint32_t mov_w0(uint32_t imm) { return 0x52800000u | ((imm & 0xFFFFu) << 5); }
#define RET_INSN 0xD65F03C0u

// Writes `mov w0,#v; ret` through the RW alias, invalidates both aliases,
// executes at RX. Returns the value the code returned (or -1).
static int run_probe(uint8_t *rw, uint8_t *rx, uint32_t v) {
    uint32_t code[2] = {mov_w0(v), RET_INSN};
    memcpy(rw, code, sizeof code);
    sys_icache_invalidate(rw, sizeof code);
    sys_icache_invalidate(rx, sizeof code);
    int (*fn)(void) = (int (*)(void))rx;
    return fn();
}

static int selftest(const char *marker_tag, uint32_t a, uint32_t b, int *r1, int *r2) {
    void *rx = NULL;
    void *rw = rl_jit26_alloc(PG, &rx);
    if (!rw) { *r1 = *r2 = -1; return 0; }
    write_marker(marker_tag);
    *r1 = run_probe(rw, rx, a);
    *r2 = run_probe(rw, rx, b); // write-after-execute: a JIT patches code it already ran
    clear_marker();
    rl_jit26_free(rw, PG);
    return *r1 == (int)a && *r2 == (int)b;
}

int rl_jit26_selftest_json(char *out, size_t cap) {
    if (!g.ready) {
        snprintf(out, cap, "{\"ok\":false,\"error\":\"pool not ready\",\"stage\":\"%s\"}", g.stage);
        return 0;
    }
    int r1, r2;
    int ok = selftest("manual-test", 42, 43, &r1, &r2);
    rl_logf("jit26: self-test %s: %d %d (ptraced=%d)", ok ? "ok" : "FAILED", r1, r2, rl_is_ptraced());
    snprintf(out, cap,
             "{\"ok\":%s,\"result1\":%d,\"result2\":%d,\"ptraced\":%d,\"detached\":%s,\"pool\":\"0x%llx\","
             "\"rw_alias\":\"0x%llx\"}",
             ok ? "true" : "false", r1, r2, rl_is_ptraced(), g.detached ? "true" : "false",
             (unsigned long long)(uintptr_t)g.rx, (unsigned long long)(uintptr_t)g.rw);
    return ok;
}

// ---------------------------------------------------------------- allocator

static void segs_init(void) {
    g.nsegs = 1;
    g.segs[0].off = 0;
    g.segs[0].len = g.size;
    g.segs[0].used = 0;
}

void *rl_jit26_alloc(size_t size, void **rx_out) {
    if (rx_out) *rx_out = NULL;
    if (!g.ready || size == 0) return NULL;
    size = (size + PG - 1) & ~(size_t)(PG - 1);
    void *result = NULL;
    pthread_mutex_lock(&g.lock);
    for (int i = 0; i < g.nsegs; i++) {
        struct seg *s = &g.segs[i];
        if (s->used || s->len < size) continue;
        if (s->len > size && g.nsegs < MAX_SEGS) {
            memmove(&g.segs[i + 1], &g.segs[i], (size_t)(g.nsegs - i) * sizeof *s);
            g.nsegs++;
            g.segs[i + 1].off = s->off + size;
            g.segs[i + 1].len = s->len - size;
            g.segs[i + 1].used = 0;
            s->len = size;
        }
        s->used = 1;
        result = g.rw + s->off;
        if (rx_out) *rx_out = g.rx + s->off;
        break;
    }
    pthread_mutex_unlock(&g.lock);
    if (!result) rl_logf("jit26: alloc(%zu) failed: pool exhausted", size);
    return result;
}

int rl_jit26_free(void *rw, size_t size) {
    (void)size;
    if (!g.ready || !rw) return 0;
    uintptr_t p = (uintptr_t)rw;
    if (p < (uintptr_t)g.rw || p >= (uintptr_t)g.rw + g.size) return 0;
    size_t off = p - (uintptr_t)g.rw;
    pthread_mutex_lock(&g.lock);
    for (int i = 0; i < g.nsegs; i++) {
        if (g.segs[i].off != off) continue;
        g.segs[i].used = 0;
        // coalesce with the next, then the previous free neighbour
        if (i + 1 < g.nsegs && !g.segs[i + 1].used) {
            g.segs[i].len += g.segs[i + 1].len;
            memmove(&g.segs[i + 1], &g.segs[i + 2], (size_t)(g.nsegs - i - 2) * sizeof g.segs[0]);
            g.nsegs--;
        }
        if (i > 0 && !g.segs[i - 1].used) {
            g.segs[i - 1].len += g.segs[i].len;
            memmove(&g.segs[i], &g.segs[i + 1], (size_t)(g.nsegs - i - 1) * sizeof g.segs[0]);
            g.nsegs--;
        }
        break;
    }
    pthread_mutex_unlock(&g.lock);
    return 1;
}

void *rl_jit26_to_rw(const void *rx) {
    uintptr_t p = (uintptr_t)rx;
    if (g.ready && p >= (uintptr_t)g.rx && p < (uintptr_t)g.rx + g.size) return g.rw + (p - (uintptr_t)g.rx);
    return (void *)rx;
}

void *rl_jit26_to_rx(const void *rw) {
    uintptr_t p = (uintptr_t)rw;
    if (g.ready && p >= (uintptr_t)g.rw && p < (uintptr_t)g.rw + g.size) return g.rx + (p - (uintptr_t)g.rw);
    return (void *)rw;
}

void rl_jit26_pool_json(char *out, size_t cap) {
    size_t used = 0, freeb = 0, largest = 0;
    pthread_mutex_lock(&g.lock);
    for (int i = 0; i < g.nsegs; i++) {
        if (g.segs[i].used) used += g.segs[i].len;
        else { freeb += g.segs[i].len; if (g.segs[i].len > largest) largest = g.segs[i].len; }
    }
    int n = g.nsegs;
    pthread_mutex_unlock(&g.lock);
    snprintf(out, cap, "{\"used\":%zu,\"free\":%zu,\"largest_free\":%zu,\"segments\":%d,\"size\":%zu}", used, freeb,
             largest, n, g.size);
}

// ---------------------------------------------------------------- preparation

int rl_jit26_ready(void) { return g.ready; }
const char *rl_jit26_stage(void) { return g.stage; }

static void *prepare_stub(void *addr, size_t len) {
    pthread_mutex_lock(&g.brk_lock);
    void *r = JIT26PrepareRegion(addr, len);
    pthread_mutex_unlock(&g.brk_lock);
    return r;
}

int rl_jit26_detach(void) {
    pthread_mutex_lock(&g.brk_lock);
    int before = g.unserviced_traps;
    JIT26Detach();
    int serviced = (g.unserviced_traps == before);
    pthread_mutex_unlock(&g.brk_lock);
    pthread_mutex_lock(&g.lock);
    g.detached = 1;
    g.detach_rc = serviced;
    pthread_mutex_unlock(&g.lock);
    rl_logf("jit26: detach %s (ptraced now %d)", serviced ? "serviced" : "NOT serviced (no debugger?)", rl_is_ptraced());
    return serviced;
}

int rl_jit26_prepare_now(size_t pool_bytes, int wait_s, int flags) {
    pthread_mutex_lock(&g.lock);
    if (g.ready) { pthread_mutex_unlock(&g.lock); return 1; }
    if (g.in_progress) { pthread_mutex_unlock(&g.lock); return 0; }
    g.in_progress = 1;
    g.err[0] = 0;
    g.wait_s_asked = wait_s;
    pthread_mutex_unlock(&g.lock);

    size_t size = (pool_bytes + PG - 1) & ~(size_t)(PG - 1);
    if (size == 0) size = 128u << 20;

    // 1. wait for the debugger
    set_stage("waiting-for-debugger");
    double t0 = now_s();
    while (rl_is_ptraced() != 1 && now_s() - t0 < (double)wait_s) usleep(200 * 1000);
    g.waited_s = now_s() - t0;
    g.ptraced_at_prepare = rl_is_ptraced();
    rl_logf("jit26: ptraced=%d after %.1f s (asked %d s); pool %zu MB flags=%d", g.ptraced_at_prepare, g.waited_s,
            wait_s, size >> 20, flags);

    // 2. prepare
    set_stage("preparing");
    t0 = now_s();
    void *rx = NULL;
    if (flags & RL_JIT26_IN_PLACE) {
        g.in_place_tried = 1;
        void *pool = mmap(NULL, size, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (pool == MAP_FAILED) {
            g.in_place_errno = errno;
#ifdef MAP_JIT
            pool = mmap(NULL, size, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
#endif
        }
        if (pool != MAP_FAILED) {
            rl_logf("jit26: in-place pool mapped at %p, asking the debugger to prepare it", pool);
            rx = prepare_stub(pool, size);
            if (rx) {
                g.in_place_prepared = 1;
                g.prepared_by = "app-pool";
                if (rx != pool) rl_logf("jit26: note: debugger returned %p for pool %p", rx, pool);
            } else {
                rl_logf("jit26: in-place prepare failed; falling back to a debugger-allocated region");
                munmap(pool, size);
            }
        } else {
            rl_logf("jit26: in-place pool mmap failed errno=%d; falling back", g.in_place_errno);
        }
    }
    if (!rx) {
        rx = prepare_stub(NULL, size);
        if (rx) g.prepared_by = "debugger";
    }
    g.prepare_s = now_s() - t0;
    if (!rx) {
        set_err("PrepareRegion returned 0 (ptraced=%d, unserviced traps=%d, waited %.1f s)", rl_is_ptraced(),
                g.unserviced_traps, g.waited_s);
        set_stage("failed");
        pthread_mutex_lock(&g.lock); g.in_progress = 0; pthread_mutex_unlock(&g.lock);
        return 0;
    }
    rl_logf("jit26: prepared %zu MB at %p by %s in %.2f s", size >> 20, rx, g.prepared_by, g.prepare_s);

    // 3. RW alias
    set_stage("remapping");
    t0 = now_s();
    vm_address_t rw = 0;
    vm_prot_t cur = VM_PROT_NONE, max = VM_PROT_NONE;
    g.remap_kr = vm_remap(mach_task_self(), &rw, (vm_size_t)size, 0, VM_FLAGS_ANYWHERE, mach_task_self(), (vm_address_t)rx,
                          FALSE, &cur, &max, VM_INHERIT_NONE);
    if (g.remap_kr != KERN_SUCCESS) {
        set_err("vm_remap failed: %d (%s)", (int)g.remap_kr, mach_error_string(g.remap_kr));
        set_stage("failed");
        pthread_mutex_lock(&g.lock); g.in_progress = 0; pthread_mutex_unlock(&g.lock);
        return 0;
    }
    g.protect_kr = vm_protect(mach_task_self(), rw, (vm_size_t)size, FALSE, VM_PROT_READ | VM_PROT_WRITE);
    if (g.protect_kr != KERN_SUCCESS) {
        set_err("vm_protect(RW) failed: %d (%s) cur=%d max=%d", (int)g.protect_kr, mach_error_string(g.protect_kr), (int)cur,
                (int)max);
        vm_deallocate(mach_task_self(), rw, (vm_size_t)size);
        set_stage("failed");
        pthread_mutex_lock(&g.lock); g.in_progress = 0; pthread_mutex_unlock(&g.lock);
        return 0;
    }
    g.remap_s = now_s() - t0;
    rl_logf("jit26: RW alias at 0x%llx (cur=%d max=%d) delta=%lld", (unsigned long long)rw, (int)cur, (int)max,
            (long long)((intptr_t)rw - (intptr_t)rx));

    pthread_mutex_lock(&g.lock);
    g.rx = rx;
    g.rw = (uint8_t *)rw;
    g.size = size;
    segs_init();
    g.ready = 1; // the allocator is usable from here on
    pthread_mutex_unlock(&g.lock);

    // 4. self-tests
    if (!(flags & RL_JIT26_NO_SELFTEST)) {
        set_stage("testing");
        int r1, r2;
        int ok = selftest("attached-test", 42, 43, &r1, &r2);
        g.attached_test = ok;
        rl_logf("jit26: attached self-test %s: %d %d", ok ? "ok" : "FAILED", r1, r2);
        if (!ok) {
            set_err("attached self-test returned %d/%d (expected 42/43)", r1, r2);
            pthread_mutex_lock(&g.lock); g.ready = 0; g.in_progress = 0; pthread_mutex_unlock(&g.lock);
            set_stage("failed");
            return 0;
        }
        if (!(flags & RL_JIT26_NO_DETACH)) {
            set_stage("detaching");
            write_marker("detach");
            rl_jit26_detach();
            usleep(300 * 1000);
            clear_marker();
            set_stage("testing");
            ok = selftest("detached-test", 44, 45, &r1, &r2);
            g.detached_test = ok;
            rl_logf("jit26: detached self-test %s: %d %d (ptraced=%d)", ok ? "ok" : "FAILED", r1, r2, rl_is_ptraced());
            if (!ok) set_err("detached self-test returned %d/%d", r1, r2);
        }
    }
    set_stage("ready");
    pthread_mutex_lock(&g.lock); g.in_progress = 0; pthread_mutex_unlock(&g.lock);
    return 1;
}

struct start_args { size_t bytes; int wait_s, flags; };

static void *start_thread(void *p) {
    struct start_args a = *(struct start_args *)p;
    free(p);
    pthread_setname_np("rl.jit26.prepare");
    rl_jit26_prepare_now(a.bytes, a.wait_s, a.flags);
    return NULL;
}

int rl_jit26_start(size_t pool_bytes, int wait_s, int flags) {
    pthread_mutex_lock(&g.lock);
    if (g.started) { pthread_mutex_unlock(&g.lock); return -EEXIST; }
    g.started = 1;
    pthread_mutex_unlock(&g.lock);
    struct start_args *a = malloc(sizeof *a);
    if (!a) return -ENOMEM;
    a->bytes = pool_bytes; a->wait_s = wait_s; a->flags = flags;
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&t, &attr, start_thread, a);
    pthread_attr_destroy(&attr);
    if (rc != 0) { free(a); g.started = 0; return -rc; }
    return 0;
}

// ---------------------------------------------------------------- status

static const char *tri(int v) { return v < 0 ? "null" : (v ? "true" : "false"); }

void rl_jit26_status_json(char *out, size_t cap) {
    char pool[160];
    rl_jit26_pool_json(pool, sizeof pool);
    pthread_mutex_lock(&g.lock);
    snprintf(out, cap,
             "{\"ok\":%s,\"stage\":\"%s\",\"pool\":\"0x%llx\",\"rw_alias\":\"0x%llx\",\"size\":%zu,\"size_mb\":%zu,"
             "\"prepared_by\":\"%s\",\"attached_test\":%s,\"detached_test\":%s,\"detached\":%s,\"detach_serviced\":%s,"
             "\"wait_s\":%d,\"waited_s\":%.1f,\"prepare_s\":%.2f,\"remap_s\":%.3f,\"ptraced_at_prepare\":%d,\"ptraced\":%d,"
             "\"cs_debugged\":%d,\"unserviced_traps\":%d,\"in_place\":{\"tried\":%s,\"errno\":%d,\"prepared\":%s},"
             "\"remap_kr\":%d,\"protect_kr\":%d,\"allocator\":%s,\"error\":\"%s\"}",
             g.ready ? "true" : "false", g.stage, (unsigned long long)(uintptr_t)g.rx, (unsigned long long)(uintptr_t)g.rw,
             g.size, g.size >> 20, g.prepared_by, tri(g.attached_test), tri(g.detached_test), g.detached ? "true" : "false",
             g.detached ? (g.detach_rc ? "true" : "false") : "null", g.wait_s_asked, g.waited_s, g.prepare_s, g.remap_s,
             g.ptraced_at_prepare, rl_is_ptraced(), rl_cs_debugged(), g.unserviced_traps, g.in_place_tried ? "true" : "false",
             g.in_place_errno, g.in_place_prepared ? "true" : "false", (int)g.remap_kr, (int)g.protect_kr, pool, g.err);
    pthread_mutex_unlock(&g.lock);
}

// ---------------------------------------------------------------- SIGTRAP guard

int rl_jit26_sigtrap_guard(void *uctx) {
#if defined(__arm64__) || defined(__aarch64__)
    ucontext_t *uc = (ucontext_t *)uctx;
    if (!uc || !uc->uc_mcontext) return 0;
    uintptr_t pc = (uintptr_t)__darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
    uintptr_t s1 = (uintptr_t)&JIT26PrepareRegion, s2 = (uintptr_t)&JIT26Detach;
    if (!((pc >= s1 && pc < s1 + 16) || (pc >= s2 && pc < s2 + 16))) return 0;
    uint32_t insn = *(const uint32_t *)pc; // our own text: always readable
    if (insn != 0xD43E01A0u /* brk #0xf00d */ && insn != 0xD4200D20u /* brk #0x69 */) return 0;
    uc->uc_mcontext->__ss.__x[0] = 0;
    __darwin_arm_thread_state64_set_pc_fptr(uc->uc_mcontext->__ss, (void *)(pc + 4));
    __atomic_fetch_add(&g.unserviced_traps, 1, __ATOMIC_RELAXED);
    return 1;
#else
    (void)uctx;
    return 0;
#endif
}
