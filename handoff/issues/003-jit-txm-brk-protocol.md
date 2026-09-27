# 003 — JIT on tvOS 27 / A15: the StikDebug "prepare region" (brk #0xf00d) protocol — complements 001-jit.md

Opened by LAPTOP, 2026-09-27 (first pushed as 001-jit-tvos27-txm.md; renumbered to avoid clashing
with REPO's 001-jit.md, which it complements).

**Relation to REPO's 001-jit.md:** REPO's arena approach (app maps an arena, tv.py writes
every 16 KB page through debugserver, then the app writes code into the arena and executes it)
is the same TXM "one debugger write per page" rule. What every working TXM app does in
addition is keep the region **RX** and write code through a **vm_remap'd RW alias**, with the
debugger doing the allocation/blessing on request (below). If request 001 shows that arena
pages can't be written and executed by the app after authorization (SIGKILL on W→X, or
`rwx_errno` set), switch to this contract. The laptop side supports both.

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

## Laptop half (LAPTOP builds it)
REPO's tools/tv.py `jit` (gdbremote backend) speaks gdb-remote itself; LAPTOP's job is a
debugserver address it can reach without root (`DEBUGSERVER_ADDR` / `DEBUGSERVER_CMD`):
- pymobiledevice3's rootless `--userspace` tunnel starts from a lockdown/USB connection per
  its source, so it probably cannot reach a USB-less Apple TV; `remote start-tunnel` /
  `tunneld` refuse to run without root. (To be confirmed when the user allows TV use.)
- Plan: a small Rust tool on jkcoxson/idevice: RemotePairing tunnel over Wi-Fi with atvloadly's
  existing pairing record (`RpPairingFile` format; userspace TLS-PSK + jktcp, no TUN/root),
  RSD → `com.apple.internal.dt.remote.debugproxy`, exposed on `127.0.0.1:<port>` and printed as
  `connect://127.0.0.1:<port>` for `DEBUGSERVER_CMD`; heartbeat kept alive meanwhile.
  DDI via atvloadly's `plumesign mount` (bitxeno tvOS_DDI; manifest lists j255ap; atvloadly's
  screenshot already works on this TV).
- For the brk protocol the same port serves a small stay-attached loop (x16=1 → `_M`/bless,
  x16=0 → detach); tv.py or the tool can run it.

## References
- StikJIT INTEGRATION.md: https://github.com/StikDebug/StikJIT/blob/main/INTEGRATION.md
- universal.js / legacy.js: https://github.com/StikDebug/StikDebug/tree/main/StikDebug/Scripts
- TXM device list: StikDebug/Support/ProcessInfo+TXM.swift
- debugserver `_M`: llvm-project lldb/tools/debugserver/source/MacOSX/MachTask.mm
- App examples: RetroArch pkg/apple/JITSupport.m (also has a tvOS LAN-helper client),
  DolphiniOS MemoryUtil_iOS_LuckTXM.cpp, MeloVertex DualMappedJitAllocator.cs.
- **AetherPS4** (https://github.com/Leviidev/AetherPS4) has a FEXCore Darwin fork with RW↔RX
  address translation (runtime/sources/fexcore-darwin), a direct reference for fex/.

## Status 2026-09-27 (LAPTOP)
- REPO adopted this contract in 001-jit.md. The laptop half is built: `laptop/jit/` (Rust,
  idevice 0.1.68, crates.io) → `rltvos-jit probe|run`, wrapped by `laptop/jit.sh`
  (`JIT_BACKEND=cmd`, `JIT_CMD="$HOME/local_RLtvOS/laptop/jit.sh --pid {pid}"`).
  - Tunnel: pair-verify only, never pair-setup, so no surprise PIN on the TV. Then TLS-PSK +
    jktcp + RSD. DDI: `jit.sh` probes for debugproxy and, if it's missing, mounts via
    atvloadly `/api/devices/:id/mountimage`.
  - Loop = universal.js: `c` → stop → `m<pc>,4` → brk #0xf00d? → `P20=pc+4` →
    x16=1: `_M<len>,rx` if x0=0, `M<page>,1:69` per 16 KB (128 pipelined), `P0=addr`;
    x16=0: `D`. Other stops: `vCont;S<sig>:<tid>` (the app's own handlers run). brk #0x69 →
    x0=0xE0000069 (not supported, like universal.js).
  - `jit.sh` returns once the first region is prepared (or the app detached). The helper stays
    in the background for later PrepareRegion calls until the app detaches or exits.
  - Unit-tested offline (stop-reply parsing, LE encoding, brk decoding). **Not yet run
    against the TV: the user asked to hold all TV use.**
- tools/tv.py: `cycle` with `JIT_LAUNCHES_APP=1` kills the app, then `do_jit` waits for it
  to be up, so that path can't work yet. LAPTOP uses attach mode (`JIT_LAUNCHES_APP=0`: atvremote
  launch → app waits for a debugger (P_TRACED poll) → `tv.py jit` → `jit.sh --pid {pid}`).
  The app must therefore not call the brk stubs before `P_TRACED`, or the SIGTRAP guard fails
  them. `jit.sh --launch BUNDLE` also exists if you'd rather have JIT_CMD launch the app.

## 2026-09-27 first TV run (request 001, build-11): blocked on the tvOS 27 developer disk image
- **Tunnel works rootless:** `laptop/jit.sh --probe` does pair-verify with atvloadly's
  RemotePairing record, TLS-PSK tunnel, jktcp and RSD, and lists 60 services in 0.2 s. No PIN,
  nothing on screen (results/001-harness/jit-probe.json).
- **No debug services:** `com.apple.internal.dt.remote.debugproxy` and
  `com.apple.coredevice.appservice` are absent until a developer disk image (DDI) is mounted.
  Present: `com.apple.security.cryptexd.remote`, `com.apple.mobile.mobile_image_mounter.shim.remote`,
  `com.apple.instruments.dtservicehub`.
- **Classic personalized DDI fails on tvOS 27:** atvloadly/plumesign with bitxeno's tvOS_DDI
  (Xcode 26 era) gives `failed to mount personalized image: ImageMountFailed`.
- **tvOS 27 needs the Cryptex1 DDI** (pymobiledevice3: "Only install the cryptex DDI from
  iOS 27", CRYPTEX_IMAGE_MIN_VERSION 27.0; idevice has `cryptexd::install_ddi`). The only public
  Cryptex DDI (doronz88/DeveloperDiskImage, iOS, build 27A5228h) is `Cryptex1,ProductClass
  0xF2` "iOS Customer Developer Disk Image Cryptex". Its 141 identities cover every other
  A15 board but not J255AP, and doronz88's `update_ddi.py --platform tvOS` notes "only iOS is
  published today". The tvOS Cryptex DDI is only in **Xcode 27**
  (`/Library/Developer/CoreDevice/CandidateDDIs/tvOS_DDI.dmg`, or its source in the Xcode bundle).
- **App side:** without a debugger the app did exactly what it should: `stage: failed`,
  `PrepareRegion returned 0 (ptraced=0, unserviced traps=1, waited 60.1 s)`, still running.
- **Unblocking, asked of the user:**
  (a) a Mac with Xcode 27 → copy `tvOS_DDI.dmg`, or run doronz88's
      `update_ddi.py --platform tvOS --variant cryptex`, and send the files; or
  (b) the user downloads Xcode 27 (.xip, ~10 GB, their Apple ID) to the laptop → LAPTOP
      extracts only the tvOS DDI on Linux (xar + pbzx + cpio, then 7-Zip for the DMG).
  Then LAPTOP adds `cryptexd` install (idevice `install_ddi`, TSS personalization) to
  laptop/jit and re-runs request 001. No app changes are needed for this.
- Skeleton impact: none yet. The debugger route exists; it only needs Apple's tvOS 27 DDI.

## 2026-09-27 02:22 — SOLVED: JIT works on tvOS 27 / A15 (results/001-harness, PASS)
- tvOS 27 DDI = Xcode 27's **tvOS Cryptex1 DDI** (build 27A9269, ProductClass 0xF4), taken from
  GitHub Actions `runs-on: xcode-27` (`.github/workflows/laptop-tvos-ddi.yml`, doronz88's
  update_ddi.py layout, 5-day artifact; LAPTOP keeps a copy in ~/rltvos/ddi/tvos27). Installed
  with `rltvos-jit mount-ddi` (idevice cryptexd + TSS) in 5.8 s. It stays until the TV reboots;
  `jit.sh` reinstalls it automatically when the debug services are missing.
- universal.js flow end to end: attach 1.4 s → PrepareRegion(0,128 MB) → `_M` + 8192 page writes
  (4.2 s) → Detach. App: attached 42/43, **detached 44/45**, jittest 42/43. +136 MB phys_footprint.
- In-place (`jit_in_place=1`, app-mapped RX pool prepared by the debugger) works as well.
- FEXCore JIT runs in the pool on the TV (build-15): selftest 3/7, see the result's FEX section.
- Side effect: atvloadly/plumesign installs take ~11 min while the DDI is mounted
  (RSD body 23584 bytes > 16374 read). Not ours to fix; INSTALL_TIMEOUT=1500.
