# PROGRESS

Nothing is marked PASS here without a `handoff/results/NNN-*/verdict.md`
saying PASS. fps and peak `/mem` (phys_footprint) are recorded per step
from the result's `mem.json`.

| Step | What | Request | Verdict | Build | Peak phys_footprint | fps | Notes |
|------|------|---------|---------|-------|---------------------|-----|-------|
| A  | Harness: workflow, tv.py, debug server, JIT/VA/mem on screen | 001 | **PASS** (LAPTOP 2026-09-27, build-11 + re-run): console, MEM, VA, crash handler, VFS mount/ls/cat, and **JIT ok** (attached + detached self-tests) once the tvOS 27 Cryptex DDI from Xcode 27 (`xcode-27` Actions runner, `laptop/jit mount-ddi`) was installed | build-11 | 167.8 MB (31.8 MB idle + 128 MB JIT pool resident) | — | TV numbers: VA max contiguous 6 GB, 7 × 1 GB reservations total (design changed to one shared guest address space, kernel-design §2); sigaltstack ok; RWX mmap allowed (mapped only); no Local Network prompt; pool prepared by the debugger in 4.4 s, in-place pool works too. |
| B  | FEXCore on tvOS, bare x86-64 function | 001 step 10 → 002 | **PARTIAL** (build-15): FEX's JIT executes x86-64 code in the pool on the TV; self-test 3/7 because every run reused the first test's translation (FEX's lookup cache is shared between threads and the code page came back at the same address) — invalidation added, re-run is part of request 002 | build-15 | 167.8 MB | — | Vendored FEX @59f85d6 + Darwin patches + RW/RX dual mapping; init 7.3 ms, 16 MB code buffer from the pool. |
| C1 | static hello (write/exit_group) | — | — | — | — | — | |
| C2 | dynamic glibc hello (ld.so path) | — | — | — | — | — | |
| C3 | busybox sh pipeline + background job | — | — | — | — | — | |
| C4 | Xvfb + xdpyinfo + xeyes | — | — | — | — | — | |
| C5 | wine64 notepad under Xvfb | — | — | — | — | — | |
| D1 | vkcube via Vulkan thunk | — | — | — | — | — | |
| D2 | DXVK d3d11 sample under wine64 | — | — | — | — | — | |
| D3 | synthetic evdev device from a paired controller | — | — | — | — | — | |
| E1 | RocketLeague.exe reaches main menu | — | — | — | — | — | |
| E2 | exhibition match vs bots | — | — | — | — | — | |

## Measurements log

(one line per measured change: date, build, step, phys_footprint peak, fps, what changed)

- 2026-09-27 build-11 A: phys_footprint 31.8 → 167.8 MB (+136 MB) once the 128 MB JIT pool is prepared (the debugger writes every page → all resident); available 2066 → 1930 MB; limit~ 2098 MB.
