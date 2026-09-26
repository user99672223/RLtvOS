# 001 — JIT on tvOS 27 / A15 needs the TXM "prepare region" protocol, not RWX mmap

Opened by LAPTOP, 2026-09-27. Blocks: request 001's "JIT ok", Phase B onward.

## Facts (laptop, 2026-09-26)
- TV: AppleTV14,1 (J255AP, MN873), **tvOS 27.0 (24J361)**, DeveloperModeStatus=true.
- The user has **no working JIT method** for this TV (the brief assumed one). LAPTOP is
  building the laptop half (below); the app half is REPO's.
- On iOS/tvOS 26+ devices with TXM (A15 and newer; StikDebug treats every 27 device except
  iPad8,11/12 as TXM), an attached debugger / CS_DEBUGGED no longer makes RWX or MAP_JIT
  memory executable. "Each executable memory region must be prepared through the debug
  connection before the app executes code from it." So the brief's
  "JIT memory = RWX mmap, legal because a debugger is attached" does not hold here.
  The skeleton item (FEXCore JIT with a Darwin layer) is still fine; only the memory
  mechanism changes.
- No public report of JIT working on tvOS 26/27 on an A15 was found. Unverified until we try.

## Protocol (StikDebug `universal.js`, the de-facto standard; MeloNX/MeloTV use it)
App-side stubs, **in the main binary** (not a framework), naked functions:
```
JIT26Detach:        mov x16, #0 ; brk #0xf00d ; ret
JIT26PrepareRegion: mov x16, #1 ; brk #0xf00d ; ret     // x0 = addr or 0, x1 = len → x0 = addr (0 = fail)
```
Debugger side (LAPTOP's helper) per stop: read pc/x16/x0/x1, check the brk immediate,
step pc+4; for PrepareRegion with x0=0 allocate `_M<len>,rx` (debugserver does
mach_vm_allocate + protect RX); then write one byte per 16 KiB page (`M<addr>,1:69`,
pipelined) to bless it; return the address in x0; continue. Detach = `D`.
Legacy form `brk #0x69` with x0/x1 exists too (UTM, DolphiniOS); the helper will accept both.

## App-side contract (please implement; LAPTOP will test it when the user allows TV use)
1. At startup, before FEX threads: `mmap` one RX pool (`PROT_READ|PROT_EXEC`,
   `MAP_ANON|MAP_PRIVATE`), 128–256 MB. Every blessed page is dirty and counts against the
   ~2085 MB footprint cap.
2. Wait for the debugger: poll `P_TRACED` (sysctl KERN_PROC_PID) for up to ~60 s.
   CS_DEBUGGED stays set after a detach, so it proves nothing.
3. Arm a SIGTRAP guard (on trap at a stub: pc += 4, x0 = 0) so an unserviced brk returns
   failure instead of killing the app.
4. `JIT26PrepareRegion(pool, len)`; if it returns 0, retry `JIT26PrepareRegion(0, len)`
   (the debugger allocates).
5. `vm_remap(self, &rw, len, 0, VM_FLAGS_ANYWHERE, self, rx, FALSE, …)` and protect RW:
   write code through RW, `sys_icache_invalidate` on RX, execute at RX.
6. Self-test: emit `mov x0,#42; ret`, call it, get 42. Then `JIT26Detach()` and call it
   again. Report both in `/status` → `jit` (e.g. `{"ok":…,"attached_test":…,"detached_test":…,"pool":…}`).
   If the post-detach test fails, stay attached. Every non-ignored exception then round-trips
   over Wi-Fi to the laptop, so the helper will set `QSetIgnoredExceptions`.
7. FEX: sub-allocate every code buffer from the pool; emit via RW, compute branch targets and
   entry points at RX; never munmap/mprotect/madvise pool pages; invalidate the icache on
   the RX side; serialize brk calls across threads; set no Mach exception ports for
   breakpoints. x18 stays reserved as before.

## Laptop half (LAPTOP builds it: `laptop/jit.sh` → Rust helper on jkcoxson/idevice)
- Rootless: RemotePairing tunnel over Wi-Fi using atvloadly's existing pairing record
  (idevice `RpPairingFile` format; userspace TLS-PSK + jktcp, no TUN/root).
- DDI: atvloadly's `plumesign mount` (bitxeno tvOS_DDI; its manifest lists j255ap).
- Two modes: `--launch <bundle>` (DVT launch **suspended**, vAttach, continue) or
  `--pid <pid>`. Then it services brk requests and exits after the app's detach
  (stays in the background if the app never detaches).
- tools/tv.py: with `--launch` LAPTOP will set `JIT_LAUNCHES_APP=1` in config.env; `jit`
  then runs the in-app `jittest` as today.

## References
- StikJIT INTEGRATION.md: https://github.com/StikDebug/StikJIT/blob/main/INTEGRATION.md
- universal.js / legacy.js: https://github.com/StikDebug/StikDebug/tree/main/StikDebug/Scripts
- TXM device list: StikDebug/Support/ProcessInfo+TXM.swift
- debugserver `_M`: llvm-project lldb/tools/debugserver/source/MacOSX/MachTask.mm
- App examples: RetroArch pkg/apple/JITSupport.m (also has a tvOS LAN-helper client),
  DolphiniOS MemoryUtil_iOS_LuckTXM.cpp, MeloVertex DualMappedJitAllocator.cs.
- **AetherPS4** (https://github.com/Leviidev/AetherPS4) has a FEXCore Darwin fork with RW↔RX
  address translation (runtime/sources/fexcore-darwin), a direct reference for fex/.
