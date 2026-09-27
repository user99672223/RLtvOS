# 005-c3-c4 — build-25 on the Apple TV

**Part A (C3 remainder): PARTIAL.** First deviation: **A1**. A4 threads-test, A5 signal exits,
A6 kick check and A7 pass. Every child created by **fork** (pipelines, `&` jobs, subshells,
`$(…)`) dies on its first write to the snapshotted stack.

Why: that write is a copy-on-write write-protection fault. Darwin delivers it as **SIGBUS with
si_code 1**, the value of BUS_ADRALN. build-25's hook leaves SIGBUS/BUS_ADRALN to FEX, and FEX's
unaligned handler can't fix a plain `str`, so the guest gets SIGBUS ("Bus error", exit 135).
Children created by **vfork**, which dash uses for simple commands, are fine.

**Part B (C4): PARTIAL.** B1 (writable layer, guest-file routes) passes. B2 and B3 fail on the
same fork bug. A diagnostic run of Xvfb and xdpyinfo as separate top-level guests (no fork) gets
through AF_UNIX `bind`/`listen`/`connect`, epoll and the lock file with no ENOSYS. Xvfb then dies
at keyboard init because its `xkbcomp` fork dies.

**Second finding: each guest thread that ends by FAULT or kick leaks ~256–272 MB of address
space** (FEX's per-thread lookup cache). This instance ended at 0.75 GB; a fresh instance has
~7.5 GB (build-23's fresh probe in 004).

Build **build-25** (be86387, IPA sha256 de33ee32…), app pid 1225, TV 05:57–06:05 CEST.

## Prep
- `refs/guest/*.sh` copied to `/opt/rl/refs` and the manifest rebuilt (70,388 entries).
- install: 17 s. launch `--fresh`, `jit` ready (4.26 s), `vfs mount` ok.
- c3/c4/c5.summary.txt are in this directory (redaction checked).

## Part A
| step | result |
|---|---|
| A1 `echo one \| tr o 0` | **FAIL**: `Bus error` ×2, exit 135. Both pipeline children (pids 2, 3) die with `FAULT signal=10 code=1 … -> killed by signal 7`. Details below |
| A2 `sleep 1 & …; wait; echo waited` | prints `bg=5` / `waited`, exit 0, **but the background child (pid 5) died the same way** before exec'ing `sleep`: the run took 84 ms instead of ~1 s. Deviates |
| A3 `c3.sh` | exit 0 with 11× `Bus error` (every fork child). Correct: `C3 start pid=6`, `got-TERM`, `after self-kill`, `C3 done`. Wrong: `false exit status: ` is empty, `no /proc/self/status` (the `cat \| head` children died), `threads-test exit=135` (its fork died). Files: c3.json, c3-stdout.txt, c3-log.txt, c3-shot.png, c3-mem.json |
| A4 `threads-test` | **PASS**: `threads: counter=400000 (expect 400000) joined=60 (expect 60) usr1=1 tid=18`, exit 0, 130 ms |
| A5 | **PASS**: `kill -9 $$` → exit 137 (`term_signal 9`), no output. The TERM trap → `caught`, exit 5 |
| A6 kick | **PASS**: `exec --wait 5` of the busy loop timed out; `ps --killall` → `stopped: 1`; 3 s later pid 25 was `dead` with `live_threads 0`; log: `kernel: pid 25 tid 25 kicked out of JIT code (guest rip=0x10585f9f0) -> exit code=0`. Nit: a killed process records exit 0 / `term_signal 0`, not SIGKILL |
| A7 | **PASS**: pids 1–25 `exited`/`dead`, `mapped_bytes 0`, `live_threads 0`, and children carry the shell's pid as ppid (ps-a.json). mem-a: 201 MB (peak 204 MB) |

**A1 details** (a1-log.txt lines 90–97):
```
90 25462.6 [1] pipe2(0x11a8efb20, 0x0) = 0
91 25463.0 [1] rt_sigprocmask(0, 0x11c7746f8, 0x11a8ef990, 8) = 0
92 25463.3 kernel: pid 1 fork -> child pid 2 (snapshot)
93 25463.3 kernel: pid 2 tid 2 start rip=0x11c6a0f82 rsp=0x11a8ef990
94 25463.4 [2] set_robust_list(0x11c7b8a20, 24) = 0
95 25463.6 fex[error]: Unhandled JIT SIGBUS: PC: 0x1061675f8 Instruction: 0xf81f8d14
96 25463.6 kernel: pid 2 tid 2 FAULT signal=10 code=1 pc=0x1061675f8 (in JIT code) addr=0x11a8ef988 (last block-exit guest rip=0x11c6a0fbc, unaligned fixups so far=10) -> killed by signal 7
97 25463.8 [1] clone(0x1200011, 0x0, 0x0, 0x11c7b8a10, 0x0) = 2
```
- **0xf81f8d14 = `str x20, [x8, #-8]!`**. That is FEX's `push`: x8 is the guest RSP (rsp
  0x11a8ef990 − 8 = 0x11a8ef988).
- It is a plain store to an **8-byte-aligned** address, in the top page of the shell's stack
  (`stack 0x11a0f0000-0x11a8f0000`), which the fork snapshot write-protected. So this is not an
  alignment fault: it is the copy-on-write fault, reported by Darwin as SIGBUS **code 1**.
- The same instruction and code kill every fork child in A2, A3, B2, B3 and in Xvfb's `xkbcomp`
  fork. 20 `Unhandled JIT SIGBUS` lines in all (log-full.txt).
- vfork children are unaffected. B1's `cat`/`mkdir`/`chmod`/`ls`/`rm`/`df` and B2's `sleep 0.5`s
  all ran (ps-b.json).

**Suggestion (REPO decides):** classify from the thread state instead of `si_code`:
- `uc_mcontext->__es.__esr` DFSC bits[5:0]: `0x21` is an alignment fault; `0x0D`–`0x0F` are
  permission faults (levels 1–3). WnR (bit 6) marks a write. `__es.__far` holds the address.
- Alternatively: a tracked, write-protected page plus a write → CoW; only after that, alignment →
  FEX.

## Part B
| step | result |
|---|---|
| B1 | **PASS**: `hi`; `/dev/shm`, `/run` (with `lock`, `user`) and `/tmp` listed with `.X11-unix` as `drwxrwxrwt`; `x`; after `rm`, `ls /tmp` shows no `t`; `df /tmp` → `tmpfs 50331648 33554432 16777216 67% /tmp` (odd sizes). `guest-ls /tmp` → `.X11-unix` mode 1777; `guest-put` 503 B then `guest-file` → **identical** (b1-roundtrip.diff empty) |
| B2 | **FAIL** (fork): the `Xvfb … &` child (pid 36) and both halves of `xdpyinfo \| head` died with SIGBUS, so there was no `X1` socket; `kill: No such process`; `rc=0`. There is no /tmp/xvfb1.log, because the redirect happens in the dead child |
| B3 `c4.sh` | **FAIL** (fork): line 4 `. "$(dirname "$0")/_lib.sh"` — the `$(dirname …)` child dies, so `. /_lib.sh: No such file`, exit 2. No xvfb.log and no c4.xwd |
| B4 | ps-b: all 51 processes (pids 1–55; 19–22 are threads-test threads) `exited`/`dead`, `mapped_bytes 0`, `live_threads 0`. **va-end 1.25 GB** (see below). mem-end 231 MB, peak 444 MB |

**Diagnostic, not in the request:** the same programs as separate top-level guests (no fork).
- **Xvfb** (`tv.py exec --wait 0 -- /usr/bin/Xvfb :1 -screen 0 320x240x24 -nolisten tcp
  -extension MIT-SHM`, pid 52):
  - `epoll_create1(O_CLOEXEC) = 3`;
  - `socket(AF_UNIX, SOCK_STREAM) = 6`, `bind` (abstract, len 20), `listen(6, 4096)`;
  - `socket = 7`, `unlink("/tmp/.X11-unix/X1") = -1 ENOENT`, `bind` (len 19), `listen`;
  - `epoll_ctl` ADD/MOD for both, plus the lock file.
  - All returned 0, and `guest-ls /tmp/.X11-unix` shows `X1` as a socket.
- **xdpyinfo -display :1** (pid 53):
  - `socket(1, SOCK_STREAM|SOCK_CLOEXEC) = 3`, `connect(3, {AF_UNIX, "@/tmp/.X11-unix/X1"}, 20)
    = 0`, `getpeername = 0`, `fcntl` O_NONBLOCK, `poll = 1`, `writev(3, …, 2) = 12`,
    `recvfrom = -1 EAGAIN`.
  - 2.4 s later: `poll = 1`, `recvfrom = 0` (EOF), so `unable to open display ":1"`.
- **Why the EOF:** Xvfb spent ~15 s loading Mesa/DRI libraries over the VFS and never accepted
  the connection. At keyboard init it forked twice for `xkbcomp` (pids 54, 55); both children
  died of the fork SIGBUS. Xvfb then printed `Keyboard initialization failed …` / `Fatal server
  error: Failed to activate virtual core keyboard: 2`, closed and unlinked its sockets, and
  exited 1. (`epoll_ctl(DEL)` after `close()` gives EBADF, which is harmless.)
- No ENOSYS on the socket, epoll or file calls used so far.
- All of this is in log-full.txt: Xvfb from line ~4100, xdpyinfo from 4582.

## Address space leaks ~256–272 MB per guest thread that ends by FAULT or kick
- `va --probe2`: this instance was down to **1.25 GB** at B4, although every process showed
  `mapped_bytes 0`. A fresh instance has ~7.5 GB (build-23's fresh probe in 004; I didn't probe
  build-25 fresh).
- **Memory-map walk** (`rltvos-jit regions` above the JIT pool): besides the shared cache, **42 ×
  128 MB + 24 × 16 MB read-write = 5.63 GB**. That is the shape of FEX's per-thread `LookupCache`:
  `VirtualMemSize/4K×8` (128 MB at 64 GB) + `CODE_SIZE` 128 MB + L1 16 MB = 272 MB, which XNU
  shows as 128 + 128 + 16 MB. 5.63 GB ≈ 21 of them, and 21 guest threads ended by FAULT
  (20 SIGBUS) or kick (1) in this session.
- **Direct test:** VA 1.25 GB → rerun A1 (two children FAULT) → **0.75 GB**. A clean `env`
  afterwards → still 0.75 GB (diag-va-*.json).
- So normal exits free the thread, but the fault/kick path skips `DestroyThread`. build-18's code
  says "The FEX thread object is left alone: its state is unknown after a longjmp", so its lookup
  cache (plus call-ret stack etc.) stays reserved.
- **Suggestion:** destroy the FEX thread (or at least free its `LookupCache`) once the host thread
  has longjmp'd out of JIT code. Otherwise ~25 faulting children exhaust the ~7 GB budget.

## Odd things
- **`tv.py log --all` stops at 4,000 lines** (reply `count 4000, next 4001`) without saying the log
  is longer. log-full.txt (5,150 lines) was fetched with `--since 1 --max 100000`.
- `df /tmp` numbers look synthetic (50 GB tmpfs, 67 % used).
- Guest clock: file times show UTC (`Sep 27 03:59` at 05:59 CEST), which is expected.

## Files
Part A: a1/a2 `.json`/`-stdout.txt`, a1-log.txt, c3.json, c3-stdout.txt, c3-log.txt,
c3-shot.png, c3-mem.json, threads.json/-stdout.txt, a5a/a5b `.json`/`-stdout.txt`, a6.json,
a6-killall.json, a6-ps.json, a6-log.txt, ps-a.json, mem-a.json.
Part B: b1.json/-stdout.txt, b1-ls.json, b1-put.json, b1-roundtrip.diff (empty),
b2.json/-stdout.txt, b2-log.txt, c4.json, c4-stdout.txt, c4-log.txt, c4-shot.png, ps-b.json,
va-end.json, mem-end.json.
Diagnostics: diag-regions-summary.txt, diag-xvfb.json, diag-x11-unix-ls.json, diag-xdpyinfo.json/-stdout.txt,
diag-a1-again.*, diag-env.*, diag-va-before-a1 / after-a1 / after-env.json.
Also log-full.txt, c3/c4/c5.summary.txt, launch.json, jit.json, vfs-mount.json.
Not produced: b2-xvfb1.log, c4-xvfb.log, c4.xwd and c4.png (the processes that would write them
died).
