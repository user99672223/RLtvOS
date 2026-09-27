# PROGRESS

Nothing is marked PASS here without a `handoff/results/NNN-*/verdict.md`
saying PASS. fps and peak `/mem` (phys_footprint) are recorded per step
from the result's `mem.json`.

| Step | What | Request | Verdict | Build | Peak phys_footprint | fps | Notes |
|------|------|---------|---------|-------|---------------------|-----|-------|
| A  | Harness: workflow, tv.py, debug server, JIT/VA/mem on screen | 001 | **PASS except JIT** (LAPTOP 2026-09-27, build-11): console, MEM, VA, crash handler, VFS mount/ls/cat on the TV; JIT blocked on the tvOS 27 developer disk image (issue 003) | build-11 | 39.6 MB (idle app) | — | TV numbers: VA max contiguous 6 GB, 7 × 1 GB reservations total (design changed to one shared guest address space, kernel-design §2); sigaltstack ok; RWX mmap allowed (mapped only); no Local Network prompt. |
| B  | FEXCore on tvOS, bare x86-64 function | 002 (fex status/init without JIT) → JIT request | — | build-13 (FEX built + linked) | — | — | Vendored FEX @59f85d6 + Darwin patches + RW/RX dual mapping compiles and links (build-13). Execution needs the TXM pool → waits on the DDI. |
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
