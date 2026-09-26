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
