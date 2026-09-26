# Vendored FEX — provenance and local patches

Source: https://github.com/FEX-Emu/FEX commit `59f85d6b7df4eb053d2cc1b9d33411b4f31bfbaa`
(shallow clone, 2026-09-26). Vendored subset: `FEXCore/` (without unittests/docs),
`CodeEmitter/`, `FEXHeaderUtils/`, `External/{fmt,xxhash,unordered_dense,
SoftFloat-3e,cephes,rpmalloc}` (tests/docs removed), `Data/CMake/LinkerGC.cmake`,
`Source/Tools/CommonTools/DummyHandlers.*`, `LICENSE` (MIT).

FEX's root `CMakeLists.txt` is not vendored; `core/fex/CMakeLists.txt` plays
its role for tvOS and for x86-64 host compile checks.

Every local change is listed here so re-vendoring can replay it. Marker in
code comments: `RLtvOS:`.

## CMake

- `FEXCore/Source/CMakeLists.txt`: `64BitAllocator.cpp` and `JemallocDummy`
  skipped on APPLE; `FEXCore_shared` not created on APPLE (static archives
  only; Apple ld rejects LinkerGC's GNU flags).

## Sources (Darwin/tvOS host support)

Each entry: file — what — why. Paths relative to `third_party/FEX/`.

Headers (FEXCore/include, FEXHeaderUtils):

- `FEXCore/include/FEXCore/Utils/TypeDefines.h` — new `FEX_HOST_PAGE_SIZE`
  (16384 on `__APPLE__`, else 4096) next to the guest-facing `FEX_PAGE_SIZE`
  — host mmap/mprotect granularity differs from the guest's 4 K pages.
- `FEXCore/include/FEXCore/Debug/InternalThreadState.h` — `InternalThreadState`
  alignment and `InterruptFaultPage` sized/aligned to `FEX_HOST_PAGE_SIZE` —
  the fault page is mprotect'ed as a whole host page.
- `FEXCore/include/FEXCore/Utils/ThreadPoolAllocator.h` — guard page of
  `PooledAllocatorVirtualWithGuard` is one host page — 4 K mprotect fails on
  a 16 K kernel.
- `FEXCore/include/FEXCore/Utils/AllocatorHooks.h` — `<malloc.h>` include
  guarded; `VirtualDontNeed` on Apple re-mmaps the range (MAP_FIXED anonymous)
  because Darwin `MADV_DONTNEED` does not guarantee zero-fill; `VirtualTHPControl`
  is a no-op — no THP on Darwin.
- `FEXCore/include/FEXCore/Utils/WritePriorityMutex.h` — futex wait/wake
  replaced by `os_sync_wait_on_address` / `os_sync_wake_by_address_all`
  (tvOS 17.4+); no futex bitsets, so readers and writers share one wake — no
  futex syscall on Darwin.
- `FEXCore/include/FEXCore/Utils/InterruptableConditionVariable.h` — Apple
  variant of the class over `os_sync_wait_on_address(_with_timeout)` with
  `OS_CLOCK_MACH_ABSOLUTE_TIME` — same reason.
- `FEXCore/include/FEXCore/Utils/SignalScopeGuards.h` — signal-mask save/
  restore through `pthread_sigmask` with Darwin's 32-bit `sigset_t` — no
  `rt_sigprocmask` syscall, different sigset width.
- `FEXCore/include/FEXCore/Utils/PrctlUtils.h` — `<sys/prctl.h>` guarded; the
  constants stay defined for shared code — no prctl on Darwin.
- `FEXHeaderUtils/FEXHeaderUtils/Syscalls.h` — Apple branch: `getcpu` → CPU 0,
  `gettid` via `pthread_threadid_np`, `tgkill` → `ERROR_AND_DIE`, `statx` /
  `renameat2` / `pidfd_open` → `ENOSYS`, `getrandom` via `getentropy` loop —
  Linux-only syscalls.
- `FEXHeaderUtils/FEXHeaderUtils/Filesystem.h` — `<limits.h>`; file copy via
  read/write loop — Darwin `sendfile` is socket-only.

Sources (FEXCore/Source):

- `Utils/Allocator.cpp` — Apple section: `mmap`/`munmap` hook pointers default
  to libc, `VirtualName` no-op, `SetupHooks(PageSize)` only initialises the
  allocator, `GetHostVABits()` = 47, stubs for `CollectMemoryGaps`,
  `StealMemoryRegion`, `Setup48BitAllocatorIfExists`, `ReclaimMemoryRegion`,
  `LockBeforeFork`/`UnlockAfterFork` — no `/proc/self/maps`, no 48-bit VA
  stealing, no fork on tvOS.
- `Utils/AllocatorHooks.cpp` — `<malloc.h>`/VMA-name/prctl guards in the
  rpmalloc glue; decommit uses `MADV_FREE` — Darwin.
- `Utils/Profiler.cpp` — Linux-only include guarded.
- `Interface/Core/ArchHelpers/Arm64Emitter.cpp` — x18 removed from the x86-64
  RA list; `PreserveAll_Dynamic` / `NotPreserved_Dynamic` reduced to `{r30}` on
  Apple (no `preserve_all` attribute in Apple clang for our targets) — x18 is
  the Darwin platform register and is clobbered by the kernel.
- `Interface/Core/JIT/MiscOps.cpp` — `<syscall.h>` guarded; `ProcessorID`
  emits constant 0 on Apple — no `getcpu`; RDPID/RDTSCP CPU index not
  advertised.
- `Interface/Core/JIT/JIT.cpp` — temp code buffer guard page and usable range
  in `FEX_HOST_PAGE_SIZE` units.
- `Interface/Core/CPUID.cpp` — the per-CPU index (RDPID/RDTSCP) gates are
  `!_WIN32 && !__APPLE__`.
- `Interface/Core/CodeCache.cpp` — `memcpy` instead of `mremap` — no mremap on
  Darwin (the on-disk code cache is disabled on tvOS anyway).
- `Interface/Core/SharedCodeBufferManager.cpp` — prctl guard; the guard-page
  mprotect of the shared code buffer is skipped on Apple — code lives in the
  debugger-authorised JIT arena whose pages cannot be re-protected under TXM.
- `Interface/Core/Interpreter/Fallbacks/F80Fallbacks.h` — `sin`/`cos` instead
  of `sincos` on Apple.

Still to do (tracked in `handoff/issues/001-jit.md`): every place that writes
into executable code (JIT.cpp link/delink/memcpy, Arm64 back-patches, the
dispatcher) must go through the RW alias of the TXM-prepared RX pool
(`RLtvOS::ToRW(ptr)`); not applied yet.
