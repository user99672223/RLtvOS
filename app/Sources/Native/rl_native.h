// rl_native.h — host-side probes and crash handling used by the Swift app.
// All *_json functions write a self-contained JSON object into out/cap.
#ifndef RL_NATIVE_H
#define RL_NATIVE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 1 if ptrace-attached right now (P_TRACED), 0 if not, -1 on error.
int rl_is_ptraced(void);
// 1 if the code-signing status has CS_DEBUGGED (set once a debugger has
// attached, persists after detach; this is what makes RWX/JIT legal),
// 0 if not, -1 if csops failed.
int rl_cs_debugged(void);

// Writes {"ok":bool,"method":"rwx-mmap"|"rw-then-rx",...}. Maps a fresh page
// RWX (or RW then RX), writes a function into it and — only if `execute` is
// non-zero — runs it, rewrites the page and runs it again (a JIT's
// write-after-execute pattern). With execute == 0 it only reports what the
// mappings allowed (rwx_errno, ptraced, cs_debugged). Executing code on a
// page no debugger has written to is SIGKILLed on tvOS 26+ (TXM), so
// callers pass execute != 0 only after authorization.
int rl_jit_test_json(int execute, char *out, size_t cap);

// JIT arena — one fixed RWX (or RW) anonymous region allocated at startup
// whose pages an external debugger authorizes (one write per 16 KB page);
// all JIT code later lives here. Returns 0, or -errno; EEXIST if present.
int rl_jit_arena_init(size_t size);
// {"base":"0x..","size":N,"prot":"rwx"|"rw"|"none","rwx_errno":e,"page":16384,"pages":P}
void rl_jit_arena_json(char *out, size_t cap);
// Write/exec/rewrite/exec inside arena page `page` (fresh != 0: a fresh
// mapping instead, to test unauthorized pages). madv != 0 first
// madvise(MADV_FREE)s the page (does authorization survive reclaim?).
// Only call when execution is known to be authorized. Returns 1 on success.
int rl_jit_exec_test_json(int page, int madv, int fresh, char *out, size_t cap);

// Writes {"max_contiguous_gb":N,"total_1gb_steps":M,...}: the largest single
// PROT_NONE reservation (binary search) and how many 1 GB PROT_NONE
// reservations succeed back to back (up to step_limit). Everything is
// unmapped again before returning.
void rl_va_probe_json(char *out, size_t cap, int step_limit_gb);

// Writes {"phys_footprint":..,"peak_phys_footprint":..,"resident_size":..,
// "available":os_proc_available_memory(),...}.
void rl_mem_json(char *out, size_t cap);

// Writes {"hw_machine":"AppleTVxx,y","kern_osversion":..,"hw_memsize":..,
// "hw_pagesize":..,"ncpu":..}.
void rl_sysinfo_json(char *out, size_t cap);

// Install SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGTRAP/SIGABRT handlers (on an
// alternate stack when sigaltstack works on this box) that write a backtrace
// to `path`, then re-raise. `path` is copied. Returns 0 on success.
int rl_crash_install(const char *path);

// "ok", "symbol-missing", "errno=N", ... — whether sigaltstack (SDK-prohibited
// on tvOS, resolved via dlsym) works. Meaningful after rl_crash_install.
const char *rl_altstack_status(void);

// Deliberately fault (for testing /crash): kind 0 = NULL write (SIGSEGV),
// 1 = abort(), 2 = illegal instruction. Does not return.
void rl_crash_now(int kind) __attribute__((noreturn));

// Executes `code_words` (arm64 machine code, 4-byte words) in an RWX page
// as int(*)(void) and returns its result; sets *err to an errno on mmap
// failure (result then undefined). Used by later phases' smoke tests.
int rl_exec_words(const uint32_t *code_words, size_t n_words, int *err);

#ifdef __cplusplus
}
#endif
#endif
