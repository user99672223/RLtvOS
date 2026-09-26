// rlfex.h — C API of the FEXCore-on-Darwin layer, used by the Swift app.
// Phase B: run a bare x86-64 function (no kernel) and report the result.
#ifndef RLFEX_H
#define RLFEX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Executable memory provider for the JIT: the app's TXM-authorised arena.
// Must be set before rlfex_init. alloc returns page-aligned (16 KB) RWX
// memory or NULL; free may be a no-op.
typedef void *(*rlfex_exec_alloc_fn)(size_t size);
typedef void (*rlfex_exec_free_fn)(void *ptr, size_t size);
void rlfex_set_exec_allocator(rlfex_exec_alloc_fn alloc, rlfex_exec_free_fn free_fn);

// Log sink (one line per call). Optional; defaults to stderr.
typedef void (*rlfex_log_fn)(const char *line);
void rlfex_set_log(rlfex_log_fn fn);

// Initialise FEXCore (config, allocator, host features, context). Writes a
// JSON object {"ok":bool,"version":..,"host_features":{..},"error":..}.
// Returns 1 on success. Idempotent.
int rlfex_init(char *out, size_t cap);

// Run x86-64 machine code (must end with `hlt`; RAX at hlt is the result) on
// a fresh guest thread with a 1 MB stack. RDI/RSI/RDX are the arguments.
// Writes {"ok":bool,"rax":N,"rip":..,"ms":..,"exit":"hlt"|"fault",...}.
int rlfex_run_bare(const uint8_t *code, size_t code_len, uint64_t rdi, uint64_t rsi, uint64_t rdx,
                   char *out, size_t cap);

// Built-in Phase B test programs: "add" (rdi+rsi+rdx), "loop" (sum 1..rdi),
// "sse" (float mul), "call" (call/ret + stack), "mem" (store/load), "all".
int rlfex_selftest(const char *which, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
