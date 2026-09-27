# 006-c3-c4-faults — build-27 on the Apple TV

- **Part D (cpubench on the TV): PASS.** Two complete runs. FEX on the A15 roughly matches the
  laptop's native Tiger Lake: total 2.09 s vs 2.44 s. Ratios are below.
- **Part A (C3): PASS.**
  - A1 prints `0ne`; A2 prints `bg=7` / `waited` after 1.14 s.
  - A3 `c3.sh` prints every expected line, exit 0, with no FAULT or unhandled-SIGBUS line.
  - A4 is −0.25 GB.
- **Part B (C4): PARTIAL.** Xvfb now starts inside the guest (including `xkbcomp` via fork),
  its socket works and xdpyinfo's report comes back.
  - **First deviation: B3 `xeyes` never gets its window.** The fake kernel's `epoll_wait` loses
    readiness: a client's request that arrives just before Xvfb re-enters `epoll_wait` is never
    reported, so the client waits forever. xdotool hits the same thing, and `c4.sh` never
    finishes.
  - B2's own ending also hangs, but for a test reason: `kill %1` returns ESRCH in a
    non-interactive dash on Linux too (verified natively).
- **Part C (fault paths): PARTIAL.** C1 (139, `kind=3`), C4 (134), C5 (kick → 137 /
  `term_signal 9`) and C6 (no leak) pass. Two deviations:
  - C2's ro-write exits 139 as expected but logs `kind=3`, not 2;
  - C8: `df /` shows the 1 GB tmpfs numbers and `df /proc` shows the 48 GB image numbers
    (swapped).
  - C3 (recorded): the guest SIGSEGV handler is not entered; exit 139.

Build **build-27** (65b0234, IPA sha256 f7180f92…), app pid 1241, TV 06:26–06:42 CEST.
- **Prep:** `crashtest` built into the rootfs by `customize.sh` (native check: `crashtest
  handler` prints `recovered: value=42 handled=1`, exit 0). Manifest rebuilt (70,396 entries).
- **Install and launch:** install in 14 s; `jit` ready (4.3 s); `vfs mount` ok.
  - The **first `tv.py launch --fresh` after waking the TV failed**: pyatv
    `NotSupportedError: launch_app is not supported` (Companion not ready yet, TV on the
    screensaver). A second `launch --fresh` worked.
- **Fresh baseline:** `va-fresh` = 7.25 GB (256 MB steps) / 7.5 GB (64 MB steps);
  `mem-fresh` phys_footprint 191 MB.

## Part D — cpubench
| test | laptop, native (avg of 2) | TV run 1 (cold) | TV run 2 (warm) | TV/laptop (run 2) |
|---|---|---|---|---|
| int_alu ns/op | 2.56 | 1.986 | 1.998 | **0.78** |
| fp_scalar ns/step | 46.4 | 59.57 | 59.58 | **1.28** |
| simd ns/elem | 0.1267 | 0.1347 | 0.1298 | **1.02** |
| memcpy GB/s | 16.6 | 30.44 | 30.60 | **0.54** (TV 1.84× faster) |
| mem_random ns/read | 87.3 | 71.87 | 71.00 | **0.81** |
| branchy ns/step | 0.268 | 0.400 | 0.388 | **1.45** |
| calls ns/call | 0.376 | 0.439 | 0.453 | **1.20** |
| total s | 2.435 | 2.09 | 2.08 | **0.85** |

- Each TV run took 2.2 s as a process: 56 syscalls, 2.18 s / 2.15 s. Translation cost is
  invisible: run 1 ≈ run 2.
- Laptop: i5-1135G7 (results/issue-004/cpubench-laptop.txt).
- Files: cpubench-tv-1/2.json/-stdout.txt.

## Part A — C3
| step | result |
|---|---|
| A1 | **PASS** `0ne`, exit 0, 66 syscalls; no FAULT line |
| A2 | **PASS** `bg=7` / `waited`, exit 0, **1,140 ms** (≥ 1 s) |
| A3 `c3.sh` | **PASS**, exit 0, 221 syscalls, 1.5 s, `grep -c 'FAULT\|Unhandled JIT' c3-log.txt` = 0. Output: `C3 start pid=8`, `background job done` (early, allowed), `3`, `c`, `b`, `got-TERM`, `after self-kill`, `false exit status: 1`, `Name:	cat`, `Umask:	0022`, `State:	R (running)`, `threads: counter=400000 (expect 400000) joined=60 (expect 60) usr1=1 tid=20`, `threads-test exit=0`, `C3 done`. **`Name: cat` is correct** (the status is `cat`'s own), and the native reference refs/c3.stdout also says `cat`; the request's `sh` was a slip |
| A4 | va-a **7.0 GB** (fresh 7.25) → −0.25 GB, inside 0.3 (64 MB steps: 7.19 vs 7.5) |

## Part B — C4
**B2**: the output shows `srwxrwxrwx … X1`, then xdpyinfo's report: `name of display: :1`,
`vendor string: The X.Org Foundation`, `X.Org version: 21.1.16`, … (b2-stdout.txt).
- Xvfb started `xkbcomp` through a forked `/bin/sh`, and both exited 0 (pids 28/29, 33/34 in
  b2-hang-ps.json).
- Then `kill %1` → **`[25] kill(-26, 15) = -1 ESRCH`** ("kill: No such process"). dash signals
  the job's *process group*, and a non-interactive `sh -c` never makes background jobs group
  leaders.
- **Native dash does the same**: `sh -c 'sleep 3 & kill %1'` → `kill(-6, SIGTERM) = -1 ESRCH`
  under strace in the rootfs. So `wait` waited for Xvfb, which never exits, and the exec timed
  out at 90 s. Use `kill $!` in the test.
- `ps --killall` stopped sh and Xvfb (exit 137 / `term_signal 9`).
- Files: b2-xvfb1.log (one harmless line: `_XSERVTransmkdir: ERROR: euid != 0,directory
  /tmp/.X11-unix will not be created.`), b2-log.txt, b2-hang-ps.json, b2-killall.json.

**B3 `c4.sh`**: the output (c4-stdout.txt) has `Xvfb up on :0 (pid 38)` and xdpyinfo's first 30
lines. (`dimensions:` lies beyond `head -30`; refs/c4.stdout doesn't have it either.) Then
`xwininfo … 0 children.`, and nothing more. The script was stuck when the 180 s wait ran out.
At that point (hang-ps.json):
- Xvfb (38) and xeyes (46, 560 syscalls) were running;
- **xdotool (50) was running, blocked**;
- xdpyinfo had exited 141 (SIGPIPE from `head`, which is normal).

**Why xeyes has no window and xdotool hangs** (hang-log.txt; the ring buffer had already dropped
the first 2,152 lines):
```
9305 238034.3 [38] writev(8, 0x11f803a60, 1) = 32        Xvfb answers xeyes (fd 8)
9306 238034.3 [38] writev(8, 0x11f803a60, 1) = 8
9307 238034.3 [38] recvmsg(8, 0x11f803860, 0x0) = -1 EAGAIN   fd 8 drained
9308 238034.3 [46] poll(0x13f41b288, 1, -1) = 1
9309 238034.3 [46] recvmsg(3, 0x13f41b1a0, 0x0) = 40     xeyes reads the reply
9312 238034.3 [46] poll(0x13f41b278, 1, -1) = 1
9313 238034.3 [46] writev(3, 0x13f41b3d0, 3) = 28        xeyes sends its next request
9314 238034.3 [38] setitimer(0, 0x11f803ab0, 0x0) = 0
9315 238034.3 [38] clock_gettime(6, 0x11f803ac0) = 0     Xvfb then enters epoll_wait
```
- **Xvfb never reads fd 8 again.** Its later `epoll_wait(3, …, 256, ~597 s)` calls keep
  returning (`= 1`) for other fds: the listener fd 6, then fd 9 for xwininfo and for xdotool.
  fd 8 is never reported, although it holds xeyes' unread 28 bytes.
- xeyes waits forever for the reply, so it never maps its window, and `xwininfo` sees 0
  children.
- **xdotool, same pattern:** `[38] recvmsg(9) = -1 EAGAIN` at 240168.4, then
  `[50] writev(3) = 8` at 240168.5. xdotool polls for a reply that never comes.
- Xorg's ospoll registers clients with `epoll_ctl` ADD+MOD **without EPOLLET** (level-triggered).
  An fd with unread data must be reported by every `epoll_wait`.
- The fake kernel apparently reports only *wake-ups* that happen while a waiter sleeps (data
  arriving between the last read and `epoll_wait` is lost), or it loses the "still readable"
  state after an event.
- **Suggestion:** in `epoll_wait`, poll the current readiness of every registered fd (non-ET)
  before sleeping, and after each wake-up; re-arm level-triggered fds while data remains.

`ps --killall` stopped sh, Xvfb, xeyes and xdotool (all exit 137 / `term_signal 9`,
`mapped_bytes 0`; ps-b.json). There is no /refs/out/xvfb.log (hang-xvfb.log is 0 bytes), and no
c4.xwd or c4.png, because the snapshot step was never reached.

**B4**: va-b 6.75 GB (64 MB steps: 7.12); mem-b phys_footprint 254 MB, **peak 449.5 MB** (Xvfb
+ xeyes + xdotool + libraries). c4-shot.png shows the console after the killall.

## Part C — crashtest
| step | result | FAULT line (c-log.txt) |
|---|---|---|
| C1 null-write | **PASS** `writing to NULL`, exit 139 (`term_signal 11`) | `signal=11 code=2 kind=3 … addr=0x0 -> killed by signal 11` |
| C2 ro-write | exit **139** as expected, **but `kind=3`, not 2** | `signal=10 code=1 kind=3 … addr=0x108748000 -> killed by signal 11` |
| C3 handler (recorded) | `writing to a read-only page 0x108748000 with a handler`, then exit 139: the handler is not entered yet | same as C2 (`kind=3`) |
| C4 abort | **PASS** `abort()`, exit 134 (`term_signal 6`); `tgkill(54, 54, 6) = 0` → `killed by signal 6 (default action)`; no FAULT line | — |
| C5 loop | **PASS**: `looping`; exec timed out at 5 s; `ps --killall` → `dead`, **exit 137 / `term_signal 9`**, `live_threads 0`; log `pid 55 tid 55 kicked out of JIT code (guest rip=0x10a5533a7) -> exit code=137` | — |
| C6 VA | **PASS** va-c 7.0 GB (va-a 7.0); 64 MB steps 7.25 (7.19) | |
| C7 | ps-c: pids 51–55 as above, all `mapped_bytes 0`; mem-c 252 MB | |
| C8 `df` | `/tmp` **ok**: `tmpfs 1048576 65536 983040 7% /tmp`. **But** `/`: `rootfs 1048576 65536 983040 7% /` (the tmpfs numbers) and `/proc`: `- 50331648 50331648 0 100% /proc` (the 48 GB image numbers). They look swapped: expected `/` 48 GB full, `/proc` 0 | |

**C2 `kind`:** the page crashtest `mmap`s read-only was never touched. A first write to a page
with no translation entry is reported by the MMU as a *translation* fault (DFSC 0x04–0x07) even
though the VMA exists and is read-only. So `kind=3` comes straight from the syndrome, and to get
Linux semantics (SEGV_ACCERR vs SEGV_MAPERR) the guest VMA has to be checked. The outcome is
already right (SIGSEGV, 139).

## Odd things
- **The log ring buffer is 8,192 lines**: `tv.py log --all` reported `count 8192, dropped 2724`
  at the end. The C4 run alone produced more than 8,000 lines, so hang-log.txt starts at
  sequence 2153 and misses Xvfb's start-up.
- The first launch after waking the TV needs a retry (above). LAPTOP now retries once.
- VA across the session, 256 MB steps: fresh 7.25 → A 7.0 → B 6.75 → C 7.0. No trend (005's
  per-fault leak is gone).
- Memory: 191 MB fresh → 208 after C3 → 254 after C4 (peak 449.5) → 252 after C.

## Files
va-fresh.json, mem-fresh.json, launch.json, jit.json, vfs-mount.json;
D: cpubench-tv-1/2.json/-stdout.txt;
A: a1/a2.json/-stdout.txt, c3.json, c3-stdout.txt, c3-log.txt, c3-shot.png, c3-mem.json,
va-a.json;
B: b2.json/-stdout.txt, b2-xvfb1.log, b2-log.txt, b2-hang-ps.json, b2-killall.json, c4.json,
c4-stdout.txt, hang-ps.json, hang-log.txt, hang-xvfb.log (empty), hang-killall.json,
c4-shot.png, ps-b.json, va-b.json, mem-b.json (no c4.xwd/c4.png/c4-xvfb.log: not produced);
C: c-null-write / c-ro-write / c-handler / c-abort / c-loop `.json`/`-stdout.txt`,
c-loop-killall.json, ps-c.json, va-c.json, mem-c.json, c-log.txt, df.json/-stdout.txt.
