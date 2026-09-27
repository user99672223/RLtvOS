# PROGRESS

Nothing is marked PASS here without a `handoff/results/NNN-*/verdict.md`
saying PASS. fps and peak `/mem` (phys_footprint) are recorded per step
from the result's `mem.json`.

| Step | What | Request | Verdict | Build | Peak phys_footprint | fps | Notes |
|------|------|---------|---------|-------|---------------------|-----|-------|
| A  | Harness: workflow, tv.py, debug server, JIT/VA/mem on screen | 001 | **PASS** (LAPTOP 2026-09-27, build-11 + re-run): console, MEM, VA, crash handler, VFS mount/ls/cat, and **JIT ok** (attached + detached self-tests) once the tvOS 27 Cryptex DDI from Xcode 27 (`xcode-27` Actions runner, `laptop/jit mount-ddi`) was installed | build-11 | 167.8 MB (31.8 MB idle + 128 MB JIT pool resident) | — | TV numbers: VA max contiguous 6 GB, 7 × 1 GB reservations total (design changed to one shared guest address space, kernel-design §2); sigaltstack ok; RWX mmap allowed (mapped only); no Local Network prompt; pool prepared by the debugger in 4.4 s, in-place pool works too. |
| B  | FEXCore on tvOS, bare x86-64 function | 002 | **PASS** (LAPTOP 2026-09-27, build-18): self-test **7/7** (add, loop, sse, call, mem, syscall, exit), init 4.6 ms, runs 0.05–2 ms | build-18 | 168.4 MB (app + JIT pool, before any guest) | — | build-15's 3/7 was the shared lookup cache re-running the first test's translation; fixed by code-range invalidation. |
| C1 | static hello (write/exit_group) | 002 | **PASS** (LAPTOP 2026-09-27, build-18): `hello from x86-64 static` / `333833500`, exit 0, 3 syscalls, 2.8 ms; KERN line `pid 4 … exit=0` on the screenshot | build-18 | 193.4 MB | — | First guest process on the TV: PIE static binary through the VFS, loader, syscall table and FEX. |
| C2 | dynamic glibc hello (ld.so path) | 002 → 003 | **FAIL** on build-18: SIGBUS after `brk` — FEX's TSO `ldapur` on an 8-byte load crossing 16 bytes in ld.so's `memcmp`; the guard reported it instead of back-patching (FEX's `HandleUnalignedAccess`). Fixed in build-19, re-run = request 003 | build-18 | 197.5 MB (peak 201.4) | — | Dry runs of hello-dyn and /usr/bin/sh (dash) load fine: ld.so mapped, 10 VMAs. |
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
- 2026-09-27 build-18 B/C1/C2: 168.4 MB before any guest → 193.4 MB after C1 (kernel FEX context + 16 MB code buffer + the guest's touched pages) → 197.5 MB after the C2 attempt; peak 201.4 MB; available 1900 MB. VA (vaprobe2): 6.25 GB reservable in 256 MB steps, 6.5 GB in 64 MB steps, the same for PROT_NONE / RW / RW+NORESERVE / vm_allocate; RLIMIT_AS unlimited.
