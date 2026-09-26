# PROGRESS

Nothing is marked PASS here without a `handoff/results/NNN-*/verdict.md`
saying PASS. fps and peak `/mem` (phys_footprint) are recorded per step
from the result's `mem.json`.

| Step | What | Request | Verdict | Build | Peak phys_footprint | fps | Notes |
|------|------|---------|---------|-------|---------------------|-----|-------|
| A  | Harness: workflow, tv.py, debug server, JIT/VA/mem on screen | 001 | open (waiting on LAPTOP; user HOLD on the TV) | build-11 | — | — | CI green since run 3. tvOS 27 + TXM: JIT via the StikDebug prepare-region protocol (app half `jit26.c`, laptop half LAPTOP's `jit.sh`); `/status.jit`. LAPTOP setup S0–S3, S5–S7 PASS on the laptop reference (no TV yet). |
| B  | FEXCore on tvOS, bare x86-64 function | 001 (step 10, optional) | — | build-11 (FEX linked only if the tvOS FEX build passed) | — | — | Vendored FEX @59f85d6 + Darwin patches + RW/RX dual mapping; `tv.py fex selftest` (add, loop, sse, call, mem, syscall, exit). |
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
