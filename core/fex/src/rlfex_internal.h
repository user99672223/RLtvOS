// rlfex_internal.h — shared between darwin_platform.cpp (what FEX's Linux
// frontend normally provides) and rlfex.cpp (the bare-function runner).
#pragma once

#include <rlfex/rlfex.h>

#include <FEXCore/Core/HostFeatures.h>

#include <pthread.h>
#include <signal.h>

#include <cstdarg>
#include <cstddef>

namespace rlfex {

// Logging (one line per call) to the sink set with rlfex_set_log.
void Log(const char* Fmt, ...) __attribute__((format(printf, 1, 2)));
void SetLogSink(rlfex_log_fn Fn);

// LogMan::Throw / LogMan::Msg handlers -> Log().
void InstallLogHandlers();

// FEXCore::Threads pointers backed by pthreads.
void InstallThreadHooks();

// FEXCore::Config: Initialize + our main layer + Load. Idempotent.
void InstallConfig();

// Host features from sysctl / EL0-readable registers (A15 and later).
FEXCore::HostFeatures FetchHostFeatures();

// Route executable VirtualAlloc requests into the app's dual-mapped pool.
// Returns false when no allocator was set.
bool InstallAllocatorHooks(rlfex_exec_alloc_fn Alloc, rlfex_exec_free_fn Free);

// One probe allocation through the hooks (allocate + register + free).
// Fills *RW/*RX with the addresses seen. Returns true when it worked.
bool ProbeExecAllocator(void** RW, void** RX);

// Host page size used for guest-visible mappings (16 KiB on Apple silicon).
size_t HostPageSize();

} // namespace rlfex

// ---- shared with the fake kernel (core/kernel/src/process.cpp) ---------------

namespace FEXCore::Core {
struct InternalThreadState;
}

struct rlfex_fault_info {
  int signal;
  int code;                    // siginfo si_code
  int kind;                    // RLFEX_FAULT_* (below): what the exception syndrome said
  uint64_t pc;                 // host pc
  uint64_t addr;               // si_addr
  bool in_jit;                 // pc inside the thread's JIT code buffer
  uint64_t unaligned_fixups;   // process-wide count of back-patched TSO accesses so far
};

// Everything FEXCore needs before a Context can be created: log handlers,
// allocator hooks + one probe allocation, thread hooks, config, host
// features, the fault guard. Idempotent. Returns false (with a message in
// err) when the JIT pool is not ready.
bool rlfex_platform_init(char* err, size_t cap);

// Valid after rlfex_platform_init returned true.
const FEXCore::HostFeatures& rlfex_host_features();

// First look at a SIGSEGV/SIGBUS while a guest runs: the kernel's chance to
// resolve it (copy-on-write page of a forked child). `kind` is one of the
// RLFEX_FAULT_* values below (classified from the exception syndrome, never
// from si_code: XNU gives every SIGBUS si_code 1). Alignment faults are never
// offered (they are FEX's to back-patch). Return true when the faulting
// access may be retried.
#define RLFEX_FAULT_UNKNOWN 0
#define RLFEX_FAULT_ALIGN 1
#define RLFEX_FAULT_PERMISSION 2   // the page exists but forbids the access (write to a copy-on-write page)
#define RLFEX_FAULT_TRANSLATION 3  // nothing mapped there
using rlfex_fault_hook_fn = bool (*)(int sig, int kind, uint64_t addr, uint64_t pc);
void rlfex_set_fault_hook(rlfex_fault_hook_fn fn);

// Interrupts a guest host thread that is executing JIT code: rlfex_run_guarded
// on that thread returns 1 with fault.signal == RLFEX_KICK_SIGNAL (the FEX
// thread object is then in an unknown state and is leaked, like after a
// fault). A thread inside the kernel or FEX's runtime ignores the kick (it
// exits at its next syscall boundary). The caller must know the thread is alive.
#define RLFEX_KICK_SIGNAL SIGUSR2
int rlfex_kick(pthread_t host_thread);

// Runs fn(arg) on this thread with the fault guard armed for FEX thread
// `thread` (may be null). A SIGBUS inside that thread's JIT code is first
// offered to FEX's unaligned-access handler (TSO ldapur/stlur crossing 16
// bytes: the instruction is back-patched and resumed). Returns 0 when fn
// returned normally, 1 when a fault (SIGSEGV/SIGBUS/SIGILL/SIGTRAP/SIGFPE)
// was caught: the thread longjmp'd out of fn and *out describes the fault.
int rlfex_run_guarded(void (*fn)(void*), void* arg, FEXCore::Core::InternalThreadState* thread, rlfex_fault_info* out);
