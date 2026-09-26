// jit26.h — JIT memory on tvOS 26+/TXM (A15+): the StikDebug "universal"
// protocol. A debugger attached over the wire services two `brk #0xf00d`
// stubs that live in this binary:
//
//   JIT26PrepareRegion(addr|0, len) -> x0 = executable address (0 = failure)
//   JIT26Detach()                    -> the debugger detaches
//
// The prepared region is RX and stays executable after the debugger detaches.
// We vm_remap a second, writable alias of the same physical pages (an
// ordinary in-task VM operation): code is written through the RW alias,
// the instruction cache is invalidated on both aliases, execution happens
// at RX. rw - rx is constant for one region. FEXCore's executable
// allocations are carved out of this pool (rl_jit26_alloc) and translated
// with rl_jit26_to_rw / rl_jit26_to_rx.
//
// Everything here is safe to call before a debugger attaches: without one,
// the SIGTRAP guard turns an unserviced `brk` into a 0 return.
#ifndef RL_JIT26_H
#define RL_JIT26_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The stubs (naked, in the main binary, 16 bytes each at most).
void *JIT26PrepareRegion(void *addr, size_t len);
void JIT26Detach(void);

enum {
    RL_JIT26_IN_PLACE = 1,   // map an RX pool ourselves and ask the debugger to prepare it
                             // (default: the debugger allocates the region)
    RL_JIT26_NO_DETACH = 2,  // stay attached after the self-test
    RL_JIT26_NO_SELFTEST = 4 // do not execute anything after preparing
};

// Path of a marker file written before each step that may kill the process
// (executing freshly prepared code, executing after detach). Optional.
void rl_jit26_set_marker_path(const char *path);

// Start the preparation on a background thread: wait up to wait_s seconds
// for P_TRACED, prepare `pool_bytes` (rounded up to 16 KiB), remap the RW
// alias, run the self-tests, detach (unless NO_DETACH). Returns 0 if the
// thread was started, -EEXIST if a preparation already ran/is running.
int rl_jit26_start(size_t pool_bytes, int wait_s, int flags);

// Same, synchronously on the calling thread. Returns 1 when ready.
int rl_jit26_prepare_now(size_t pool_bytes, int wait_s, int flags);

// 1 once the pool is prepared and the RW alias exists.
int rl_jit26_ready(void);
// "idle" | "waiting-for-debugger" | "preparing" | "remapping" | "testing" |
// "detaching" | "ready" | "failed"
const char *rl_jit26_stage(void);

// {"ok":bool,"stage":..,"pool":"0x..","rw_alias":"0x..","size":N,"prepared_by":
//  "app-pool"|"debugger","attached_test":..,"detached_test":..,"detached":bool,
//  "wait_s":..,"ptraced":..,"unserviced_traps":N,"error":".."}
void rl_jit26_status_json(char *out, size_t cap);

// Re-run the write/execute/rewrite/execute test in the pool now (the pool
// must be ready). Writes the same JSON as the status. Returns 1 on success.
int rl_jit26_selftest_json(char *out, size_t cap);

// Ask the debugger to detach (no-op when not attached). Returns 1 if the
// stub returned normally.
int rl_jit26_detach(void);

// Pool allocator (16 KiB granularity, first fit, coalescing). Returns the
// RW address and stores the RX alias in *rx_out; NULL when the pool is not
// ready or full. free returns 1 if `rw` was pool memory (never unmapped).
void *rl_jit26_alloc(size_t size, void **rx_out);
int rl_jit26_free(void *rw, size_t size);
void *rl_jit26_to_rw(const void *rx);
void *rl_jit26_to_rx(const void *rw);
// {"used":N,"free":N,"largest_free":N,"segments":N}
void rl_jit26_pool_json(char *out, size_t cap);

// Crash-handler hook: if the faulting pc is one of the two `brk` stubs
// (nobody serviced the trap), set x0 = 0, pc += 4 and return 1. `uctx` is
// the ucontext_t* the signal handler received.
int rl_jit26_sigtrap_guard(void *uctx);

#ifdef __cplusplus
}
#endif
#endif
