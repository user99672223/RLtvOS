// rlfex.h — C API of the FEXCore-on-Darwin layer, used by the Swift app.
// Phase B: run a bare x86-64 function (no kernel) and report the result.
#ifndef RLFEX_H
#define RLFEX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Executable memory provider for the JIT: the app's TXM-prepared pool
// (jit26.c). Must be set before rlfex_init. `alloc` returns the *writable*
// alias (16 KiB aligned) and stores the executable alias of the same pages in
// *rx_out, or returns NULL. `free_fn` returns the block to the pool.
typedef void *(*rlfex_exec_alloc_fn)(size_t size, void **rx_out);
typedef int (*rlfex_exec_free_fn)(void *rw, size_t size);
void rlfex_set_exec_allocator(rlfex_exec_alloc_fn alloc, rlfex_exec_free_fn free_fn);

// Log sink (one line per call). Optional; defaults to stderr.
typedef void (*rlfex_log_fn)(const char *line);
void rlfex_set_log(rlfex_log_fn fn);

// Initialise FEXCore (allocator hooks, config, host features, context,
// dispatcher). Needs a working exec allocator (one 16 KiB probe allocation
// is made and freed first). Writes {"ok":bool,"version":..,"host_features":
// {..},"error":..}. Returns 1 on success. Idempotent.
int rlfex_init(char *out, size_t cap);

// {"initialised":bool,"runs":N,"poisoned":bool,"last_error":".."}
void rlfex_status_json(char *out, size_t cap);

// Run x86-64 machine code (must end with `hlt`; RAX at hlt is the result) on
// a fresh guest thread with a 1 MB stack. RDI/RSI/RDX are the arguments.
// Writes {"ok":bool,"rax":N,"rip":..,"ms":..,"exit":"hlt"|"fault",
// "fault":{"signal":..,"pc":..,"addr":..}}. Returns 1 when the code reached hlt.
int rlfex_run_bare(const uint8_t *code, size_t code_len, uint64_t rdi, uint64_t rsi, uint64_t rdx,
                   char *out, size_t cap);

// Built-in Phase B test programs: "add" (rdi+rsi+rdx), "loop" (sum 1..rdi),
// "sse" (double mul), "call" (call/ret + stack), "mem" (store/load),
// "syscall" (getpid via the logging syscall handler), "all". Writes
// {"ok":bool,"tests":[{"name":..,"ok":..,"rax":..,"expected":..,"ms":..}]}.
int rlfex_selftest(const char *which, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
