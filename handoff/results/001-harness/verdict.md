# 001-harness — build-11 on the Apple TV

**Verdict: FAIL — only because of JIT.** The harness works on the TV (console, MEM, VA,
debug server, crash handler, VFS). JIT cannot be tested yet: **tvOS 27 exposes no
debugserver until its developer disk image is mounted, and on tvOS 27 that image is a
Cryptex1 that only ships with Xcode 27.** The Xcode 26 tvOS image (atvloadly/bitxeno)
fails to mount, and the public iOS 27 Cryptex has no Apple TV identity. Details in
handoff/issues/003-jit-txm-brk-protocol.md. The user has been asked for Xcode 27.

Date: 2026-09-27 02:05–02:15 CEST. Build: **build-11** (f584db1, "FEX linked: false"),
IPA sha256 c3c250174eb493d2d7d5658dafcbd03a620bda63d1d05d042620ec39de8f3767.

## What the TV showed
- tv-screen-after-launch.jpg (full screen via atvloadly): console on screen, blue top bar,
  orange bottom bar, green marker. "RLtvOS build-11 pid 1026 tvOS 27.0 AppleTV14,1 24J361";
  MEM phys_footprint 14.3 MB, available 2083.7 MB, limit~ 2098 MB; VA max contiguous 6 GB,
  1 GB steps 7 / 1024, range 0x10c800000–0x7180000000; JIT waiting-for-debugger;
  ARENA legacy rwx 16 MB (rwx_errno 0), probe mapped-only; FEX not linked; DBG ptraced=0
  cs_debugged=0 sigaltstack=ok; CORE ok (clang 17.0, libc++ 190102) threads=4 exceptions=1.
- shot.png (the app's own /screenshot, from `tv.py cycle`): same console, pid 1033,
  MEM 39.6 MB, available 2058.4 MB, VA 6 GB.
- **No "Local Network" prompt appeared**, not at first launch and not when the app
  connected out to the laptop (vfs mount). tv-screen-after-vfs-mount.jpg shows the console
  with `vfs: mount http://192.168.1.133:8090 → {"ok":true,…}`.

## Steps and results
| Step | Command | Result |
|---|---|---|
| 2 | `tv.py build --tag build-11` | downloaded (150 KB IPA) |
| 3 | `tv.py install --tag build-11` | INSTALL_CMD = laptop/install.sh → atvloadly MCP `install_app` with /share/ipa path; installed as `dev.rltvos.app.GBCWA7VWJ3` in ~5 s |
| 4 | `tv.py launch --fresh` | up, build-11, pid 1026 |
| 5 | `status`, `va`, `mem` | status-before-jit.json, va.json, mem-before-jit.json (phys_footprint 39.6 MB, peak 40.1 MB, available 2058.4 MB, limit_estimate 2098 MB) |
| 6 | `tv.py jit` | **FAIL**: `laptop/jit.sh --pid 1026` → "debug services unavailable after mount" (jit.json). Tunnel fine (jit-probe.json: RemotePairing pair-verify with atvloadly's record, TLS-PSK + userspace TCP + RSD, 60 services in 0.2 s) but no `com.apple.internal.dt.remote.debugproxy` / `com.apple.coredevice.appservice`: DDI not mounted. atvloadly's mount (plumesign, bitxeno tvOS_DDI from Xcode 26): `failed to mount personalized image: ImageMountFailed` (ddi-mount-error.txt, ddi-mount-reply.json). |
| 6b | app side without a debugger | status-after-jit-window.json: after the 60 s window `stage: failed`, `error: "PrepareRegion returned 0 (ptraced=0, unserviced traps=1, waited 60.1 s)"`. **The SIGTRAP guard works**: the brk returned 0 and the app kept running. |
| 7 | `tv.py cycle --tag build-11` | ok except JIT: install skipped (same sha), launch ok (pid 1033), shot/mem/status/log ok, no crash report (report.json) |
| 8 | `run -- crashtest 0` → `launch` → `crash` → `crash --clear` | crash.txt: `SIGSEGV (11) code=2 addr=0x10`, pc/lr/sp/far/esr, slide + unslid pc, symbolized backtrace `crash_handler ← rl_crash_now ← block_copy_helper ← libdispatch…`; cleared |
| 9 | `tv.py vfs mount` + `ls` / `cat` / `stat` / `stats` | **PASS**: mount 528.6 ms, 70,386 entries / 55,641 files / 48.19 GB, manifest 2.99 MB gz; `ls /` = Debian root with /game /prefix /home; `cat /etc/hostname` = "rltvos\n" (13.9 ms), /etc/os-release = Debian 13; `stat /opt/wine-stable/bin/wine` = 14,632 B mode 100755; 0 fetch errors (vfs-mount.json, vfs-reads.jsonl) |
| 10 | `tv.py fex selftest` | skipped: FEX not linked in build-11 |

## Numbers REPO asked for
- `/va`: `{"contig_errno":12,"highest":"0x7180000000","lowest":"0x10c800000","max_contiguous_gb":6,"step_limit_gb":1024,"stop_errno":12,"total_1gb_steps":7}`
- `/status.sysinfo.sigaltstack`: `ok`; hw_machine `AppleTV14,1`; kern_osversion `24J361`.
- legacy arena: `mmap(RWX)` **succeeded** without a debugger (`prot: rwx`, `rwx_errno: 0`), mapped only, never executed.
- phys_footprint before JIT 39.6 MB; after JIT: n/a (no preparation happened). waited_s 60.1, prepare_s 0, remap_s 0, prepared_by none, detach_serviced null.
- Helper log for the two brk requests: none. The helper never got a debugserver, so no `brk` was serviced.

## Odd things
- Log timestamps jump while uptime is 21 s (lines "17 981.1 host: did become active",
  "18 20851.4 run: vfs-mount …", "19 21380.9 vfs: mount …"), probably a different clock base.
- The console's JIT line reads "waited 0/60 s" during the wait; /status shows 60.1 only at the end.
