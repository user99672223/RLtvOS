// rlfex_internal.h — shared between darwin_platform.cpp (what FEX's Linux
// frontend normally provides) and rlfex.cpp (the bare-function runner).
#pragma once

#include <rlfex/rlfex.h>

#include <FEXCore/Core/HostFeatures.h>

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

// Runs fn(arg) on this thread with the fault guard armed for FEX thread
// `thread` (may be null). A SIGBUS inside that thread's JIT code is first
// offered to FEX's unaligned-access handler (TSO ldapur/stlur crossing 16
// bytes: the instruction is back-patched and resumed). Returns 0 when fn
// returned normally, 1 when a fault (SIGSEGV/SIGBUS/SIGILL/SIGTRAP/SIGFPE)
// was caught: the thread longjmp'd out of fn and *out describes the fault.
int rlfex_run_guarded(void (*fn)(void*), void* arg, FEXCore::Core::InternalThreadState* thread, rlfex_fault_info* out);
