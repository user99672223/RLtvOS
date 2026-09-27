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

Dual-mapped executable memory (tvOS 26+ TXM; modelled on AetherPS4's
`fexcore-darwin` port). The mmap hook returns the *writable* alias of a
debugger-prepared RX region; the emitter only ever sees write-side
addresses; pointers that escape as branch targets are translated with
`FEXCore::Allocator::GetExecutableAddress`, addresses recovered from a
running PC are translated back with `GetWritableAddress` before a store;
icache maintenance is issued on both aliases. Identity on other platforms.

- `FEXCore/include/FEXCore/Utils/AllocatorHooks.h` — declares
  `RegisterDualMapping`, `UnregisterDualMapping`, `GetExecutableAddress`,
  `GetWritableAddress` (real on Apple, inline identity elsewhere).
- `Utils/Allocator.cpp` — Apple section: fixed region table + the four
  functions above (mutex-protected, offset-preserving translation).
- `Interface/Core/JIT/JIT.cpp` — entry points translated to exec-side after
  the shared-buffer copy; `ClearICache` on both aliases (compile and
  `LoadCachedCode`); `DirectBlockDelinker`, `IndirectBlockDelinker`,
  `ExitFunctionLink` store through `GetWritableAddress` (call sites,
  `Record->HostCode`, jump thunks) and clean both sides.
- `Interface/Core/Dispatcher/Dispatcher.cpp` / `.h` — `DispatchPtr`,
  `CallbackPtr`, `End`, every `Ptrs.*` in `InitThreadPointers`,
  `GenerateABICall`'s return, `MakeSignalDelegatorConfig` and the now
  out-of-line `GetExitFunctionLinkerAddress` are exec-side; `DispatchRawBegin`
  keeps the write-side start for the icache clean of both aliases.
- `Interface/Core/CPUBackend.cpp` — `IsAddressInCodeBuffer` compares the
  live PC against the exec-side buffer base.
- `Utils/ArchHelpers/Arm64.cpp` — the inline JIT block header/tail is read
  through the writable alias; the unaligned-atomic back-patches (LDAR/STLR/
  LDAPUR/STLUR) store through `WPC = GetWritableAddress(PC)` and clean both.
- `Interface/Core/SharedCodeBufferManager.cpp` — `MAX_CODE_SIZE` 64 MB on
  Apple (all code buffers come out of one 128 MB pool whose pages are
  resident once prepared).

Found by the first tvOS compile (build-11):

- `FEXHeaderUtils/FEXHeaderUtils/Syscalls.h` — Apple `getrandom` is
  `arc4random_buf` (`<stdlib.h>`); the tvOS SDK has no `<sys/random.h>`.
- `FEXCore/Source/Utils/FileUtils.cpp` — Apple `getdents64` shim over
  `fdopendir`/`readdir` (packed Darwin `struct dirent` records; EINVAL +
  rewind when the buffer is too small, which the walkers already handle).
