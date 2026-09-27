# PROGRESS

Nothing is marked PASS here without a `handoff/results/NNN-*/verdict.md`
saying PASS. fps and peak `/mem` (phys_footprint) are recorded per step
from the result's `mem.json`.

| Step | What | Request | Verdict | Build | Peak phys_footprint | fps | Notes |
|------|------|---------|---------|-------|---------------------|-----|-------|
| A  | Harness: workflow, tv.py, debug server, JIT/VA/mem on screen | 001 | **PASS** (LAPTOP 2026-09-27, build-11 + re-run): console, MEM, VA, crash handler, VFS mount/ls/cat, and **JIT ok** (attached + detached self-tests) once the tvOS 27 Cryptex DDI from Xcode 27 (`xcode-27` Actions runner, `laptop/jit mount-ddi`) was installed | build-11 | 167.8 MB (31.8 MB idle + 128 MB JIT pool resident) | — | TV numbers: VA max contiguous 6 GB, 7 × 1 GB reservations total (design changed to one shared guest address space, kernel-design §2); sigaltstack ok; RWX mmap allowed (mapped only); no Local Network prompt; pool prepared by the debugger in 4.4 s, in-place pool works too. |
| B  | FEXCore on tvOS, bare x86-64 function | 002 | **PASS** (LAPTOP 2026-09-27, build-18): self-test **7/7** (add, loop, sse, call, mem, syscall, exit), init 4.6 ms, runs 0.05–2 ms | build-18 | 168.4 MB (app + JIT pool, before any guest) | — | build-15's 3/7 was the shared lookup cache re-running the first test's translation; fixed by code-range invalidation. |
| C1 | static hello (write/exit_group) | 002 | **PASS** (LAPTOP 2026-09-27, build-18): `hello from x86-64 static` / `333833500`, exit 0, 3 syscalls, 2.8 ms; KERN line `pid 4 … exit=0` on the screenshot | build-18 | 193.4 MB | — | First guest process on the TV: PIE static binary through the VFS, loader, syscall table and FEX. |
| C2 | dynamic glibc hello (ld.so path) | 002 → 003 | **PASS** (LAPTOP 2026-09-27, build-20): all 7 lines (`uname`, `pid=1 ppid=0 uid=1000 cwd=/`, `exe=/opt/rl/bin/hello-dyn`, `HOME`, `malloc 64MB ok 63`), exit 0, 43 syscalls, 452 ms; dash, busybox and env run too | build-20 | 197.5 MB (peak 207.1) | — | build-18 failed with SIGBUS in ld.so's `memcmp` (FEX's TSO `ldapur` on an unaligned 8-byte load); the guard now back-patches it (`HandleUnalignedAccess`, 15 fix-ups during hello-dyn). Result 003 also found that every finished guest left 2 GB of VA in FEX's rpmalloc heaps (4th guest dies) → system allocator on Darwin from build-23. |
| C3 | busybox sh pipeline + background job | 004 → 005 → 006 | **PASS** (LAPTOP 2026-09-27, build-27, result 006): `echo one \| tr o 0` → `0ne`; `sleep 1 &` + `wait` takes 1.14 s; `c3.sh` prints every expected line (pipeline, background job, subshell exit status, self-kill with a TERM trap, `/proc/self/status`, threads-test `counter=400000 joined=60 usr1=1`), exit 0, 221 syscalls, 1.5 s, no FAULT line; address space flat across the run | build-27 | 208 MB | — | History: build-23 VA budget + fork/exec/wait4 (result 004); build-25 threads/signals/kick/reaping PASS but every fork child died — XNU reports the copy-on-write fault as SIGBUS si_code 1 (= BUS_ADRALN) — and 272 MB of address space leaked per faulted thread (result 005); build-27 classifies faults from the exception syndrome and destroys FEX threads after a fault/kick. |
| C4 | Xvfb + xdpyinfo + xeyes | 005 → 006 → 007 | **PARTIAL** (LAPTOP 2026-09-27, build-27, result 006): Xvfb starts inside the guest (`xkbcomp` via fork), listens on `/tmp/.X11-unix/X1` and the abstract name, xdpyinfo's full report comes back over the socket; `xeyes` never gets its window and `xdotool` hangs: Xorg registers clients `EPOLLET` and the kernel's edge detection compared masks between scans, so a request that arrived after a drain and before the next `epoll_wait` was never reported. Fixed in build-28 (an edge = the file's wait queue woken since the last scan); re-run = request 007 | build-27 | 449.5 MB (Xvfb + xeyes + xdotool + libraries) | — | build-25 (result 005): B1 writable layer + guest-file routes PASS; Xvfb died at keyboard init (its xkbcomp fork = the C3 bug). Xvfb needs ~15 s to load Mesa/DRI libraries over the VFS. |
| C5 | wine64 notepad under Xvfb | — | — | — | — | — | |
| D1 | vkcube via Vulkan thunk | — | — | — | — | — | |
| D2 | DXVK d3d11 sample under wine64 | — | — | — | — | — | |
| D3 | synthetic evdev device from a paired controller | — | — | — | — | — | |
| M1 | pager probe: 4 GB `MAP_SHARED` Caches file dirtied + a working set touched, footprint flat, page-in rate | — | — | — | — | — | New checkpoint after issue 005: file-backed guest memory verified on the TV before D1. |
| E1 | RocketLeague.exe reaches main menu | — | — | — | — | — | Feasibility: issue 004/005 (below) — the game needs file-backed guest memory (M1) and a ~1.5 GB page cache; CPU is not the limit. |
| E2 | exhibition match vs bots | — | — | — | — | — | Same. |

## Feasibility (issue 004 → issue 005)

The two go/no-go numbers for E1/E2, measured early as proxies (DECISIONS
2026-09-27 "process correction"):

- **Memory** (issue 004, LAPTOP 2026-09-27, native, 720p low, textures 256 px,
  30 fps cap): RocketLeague.exe **4.1 GB private dirty anonymous** at the main
  menu, **4.3 GB** in a 1v1 bot match (all Wine processes: 4.2 / 4.4 GB Pss),
  plus **~1.1 GB GPU buffers** (DXVK: 1,136 MB allocated, 560 MB used), 30 fps.
  The TV kills the app at **~2.1 GB `phys_footprint`**, which counts anonymous,
  compressed and Metal/IOKit memory. The game's own heap is ~2× the whole
  device budget before FEX, Wine, Xvfb and the JIT cache. The one mechanism
  that can still fit it is file-backed guest memory (a `MAP_SHARED` Caches
  file is external memory, outside the footprint, paged by XNU).
  **Issue 005 (LAPTOP 2026-09-27) measured that situation on the laptop**
  (cgroup cap + NVMe swap, THP off, no virtual desktop): the game touches only
  **34–53 MB** of its heap per 2–60 s at the menu and **53–188 MB** per 2–60 s
  in a match (`clear_refs`/`Referenced`, cross-checked with smaps). Capped
  runs, 1v1 vs a Rookie bot:

  | cap | menu fps | match fps kickoff / +30 s / +60 s | swap used | swap-in over the match minute | major faults/s | outcome |
  |---|---|---|---|---|---|---|
  | 3072 MB | 29.9 | 29.9 / 29.9 / 29.9 | 2.8 GB | 0.04 MB/s (peak 0.14) | 1.8 | fine |
  | 2048 MB | 30.0 | 29.9 / 29.9 / 30.0 | 3.7 GB | 0.22 MB/s (peak 0.69) | 18.6 | fine |
  | **1536 MB** | 30.0 | **29.9 / 30.0 / 30.5** | 4.3 GB | **1.15 MB/s (peak 6.6)** | 135 | **fine, no OOM** |
  | 1024 MB | — | — | 5.5 GB peak | — | — | OOM-killed 40 s after launch (GPU shmem 0.6 GB + hot heap do not fit) |

  Load time grows with the cap (title screen at 63 s instead of ≤30 s at
  1536 MB: ~190 k major faults during start-up). **The pre-committed pass
  criterion (1536 MB: match ≥ 25 fps, swap-in < 50 MB/s, no OOM) is met with a
  large margin → the hold is lifted; E1/E2 stay the target and file-backed
  guest memory moves to the front of the plan as checkpoint M1** (a pager
  probe on the TV first: does `phys_footprint` stay flat while a 4 GB
  `MAP_SHARED` Caches file is dirtied and a 200 MB working set is touched, and
  at what page-in rate). The floor is GPU memory: DXVK's ~1.1 GB allocated /
  0.6 GB used count toward the footprint on the TV (Metal), so
  `dxvk.maxChunkSize` and the allocated-vs-used gap are the next memory items
  after M1; CPU is not the limit (below).
- **CPU** (issue 004 part B + result 006 part D): cpubench native on the laptop
  (i5-1135G7) vs FEX on the A15, warm run. TV/laptop time ratios: int_alu
  **0.78**, fp_scalar **1.28**, simd **1.02**, memcpy **0.54** (TV 30.6 GB/s),
  mem_random **0.81**, branchy **1.45**, calls **1.20**, total **0.85** (2.08 s vs
  2.44 s). Cold run = warm run (translation cost invisible at this size). **CPU
  is not the blocker**: FEX on the A15 matches the native Tiger Lake laptop
  that runs the game at 30 fps; branchy code and calls are the slowest kinds
  (1.2–1.5×). The memory question (issue 005) decides E1/E2.

## Measurements log

(one line per measured change: date, build, step, phys_footprint peak, fps, what changed)

- 2026-09-27 build-11 A: phys_footprint 31.8 → 167.8 MB (+136 MB) once the 128 MB JIT pool is prepared (the debugger writes every page → all resident); available 2066 → 1930 MB; limit~ 2098 MB.
- 2026-09-27 build-18 B/C1/C2: 168.4 MB before any guest → 193.4 MB after C1 (kernel FEX context + 16 MB code buffer + the guest's touched pages) → 197.5 MB after the C2 attempt; peak 201.4 MB; available 1900 MB. VA (vaprobe2): 6.25 GB reservable in 256 MB steps, 6.5 GB in 64 MB steps, the same for PROT_NONE / RW / RW+NORESERVE / vm_allocate; RLIMIT_AS unlimited.
- 2026-09-27 build-23 C3 partial (result 004): 191 MB fresh → 197 MB after 6 `env` guests; VA (256 MB steps) 7.5 GB fresh → 7.25 → 7.25 after 6 guests (rpmalloc gone: no per-guest loss). Fork children hang on the CoW/alignment fault loop (fixed in build-25).
- 2026-09-27 build-27 C3 PASS / C4 partial (result 006): 191 MB fresh → 208 MB after c3.sh → 254 MB after the C4 attempt (peak 449.5 MB with Xvfb + xeyes + xdotool) → 252 MB after five crashtest faults. VA (256 MB steps) 7.25 GB fresh → 7.0 → 6.75 → 7.0: no per-fault leak any more. cpubench on the TV: total 2.09 s (laptop native 2.44 s).
- 2026-09-27 build-25 C3/C4 partial (result 005): 201 MB after part A (peak 204), 231 MB at the end (peak 444 MB while Xvfb + xdpyinfo ran as top-level guests). VA (256 MB steps): 1.25 GB at the end of part B, 0.75 GB after two more faulting children — **272 MB per guest thread that ended by FAULT or kick** (FEX's per-thread lookup cache: 128 MB + 128 MB code + 16 MB L1; 42 × 128 MB + 24 × 16 MB rw regions in the map walk), 21 such threads in the session. Fix: destroy the FEX thread after a longjmp out of JIT code (build-27).
- 2026-09-27 build-20 C2 (result 003): 199.1 MB fresh → 196.2 MB after hello-dyn (reaped) → 213.5 MB after 4 guests, peak 214.8 MB, available 1884 MB. VA (vaprobe2, 256 MB steps): 7.25 GB fresh → 5.25 → 3.25 → 1.25 GB after ls, env and the pipeline: **2 GB per finished guest process**, held by FEX's rpmalloc thread heaps (48 × 128 MB rw regions in the map walk; 256 MB spans mapped as 512 MB, 4 per heap, never finalized). Fix: FEXCore uses the system allocator on Darwin (build-23; DECISIONS 2026-09-27).
