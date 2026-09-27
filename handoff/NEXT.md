# NEXT — resume notes (each side edits only its own section)

## repo

- State (2026-09-27): C1 **PASS** (build-18), C2 **PASS** (build-20, result 003);
  dash/busybox/env run; the pipeline stops at pipe2 (ENOSYS in build-20 — C3 code
  is in build-21+). FEX self-test 7/7.
- Result 003's blocker: every finished guest left 2 GB of VA in FEX's rpmalloc
  thread heaps (4th guest dies). Fixed by using the system allocator on Darwin
  (build-23, DECISIONS 2026-09-27); request 004 re-measures `va --probe2` across
  guests. The debug server re-creates its listener after a background trip.
- Landed for C3 (build-21; not yet exercised on the TV): fork/vfork/clone
  (threads) with copy-on-write snapshots for fork, execve (+ `#!` scripts,
  CLOEXEC, fresh address space, CPU-state reset in place), wait4/SIGCHLD/
  zombies, pipe2 (64 KB ring, EOF/EPIPE/SIGPIPE, O_NONBLOCK), a real futex
  (wait/wake/requeue/wake_op, timeouts), guest signals (rt_sigaction/
  procmask/sigaltstack/kill/tgkill/tkill/rt_sigsuspend/pause, x86-64
  rt_sigframe + rt_sigreturn, SA_RESTART/EINTR, default actions), interruptible
  sleeps, process reaping. Host-tested: waiter/queue, pipes, futex, loader.
- Open request: 004 = C3 on build-23 (allocator fix + C3 kernel): pipes/fork
  steps, `/opt/rl/refs/c3.sh`, `threads-test`, signal exits, plus the VA
  regression check (≥ 6 guests, `va --probe2` must not shrink).
- Landed for C4 (build-24+; host-tested, not yet on the TV): the writable
  overlay (`overlay.h`: tmpfs upper layer, whiteouts, copy-up, opaque
  /tmp /var/tmp /run /dev/shm; O_CREAT/O_TRUNC/O_APPEND, mkdir/unlink/rename/
  link/symlink/mknod/chmod/chown/utimens/truncate, memfd_create, sendfile),
  record locks (`locks.h`: POSIX/OFD/flock with real conflicts), AF_UNIX
  sockets (`socket.h`: stream/dgram/seqpacket, socketpair, fs + abstract
  names, SCM_RIGHTS, SO_PEERCRED/PASSCRED), poll/ppoll/select/pselect6/epoll/
  eventfd (`poll.h`), setitimer/alarm (`timers.cpp`), `/guest-file` +
  `/guest-ls` debug routes (`tv.py guest-file|guest-put|guest-ls`) to pull
  files the guest wrote (Xvfb log, xwd dumps). Guest scripts source `_lib.sh`
  relative to `$0`.
- Next: result 004 → fix what C3 shows, then request 005 = C4
  (`/opt/rl/refs/c4.sh`: Xvfb :0 + xdpyinfo + xeyes + xdotool; the xwd dump
  comes back through `tv.py guest-file /tmp/c4.xwd`). Draft in the repo
  session's scratchpad.
- Known gaps: signals reach a thread running JIT code only at its next
  syscall (async delivery = FEX SRA spill, later); a guest fault kills the
  process instead of raising SIGSEGV to a guest handler; exit_group does not
  stop sibling threads that never syscall; no SMC tracking; one lock around
  every rlvfs call; MAP_SHARED files are write-back copies (no cross-process
  sharing yet); FIFO opens do not block for the other end; EPOLLET reports
  rising edges only; no AF_INET; itimer VIRTUAL/PROF never fire.
- Facts: guest executables must be PIE (4 GB hard page zero); VA budget
  6.25–6.5 GB; `/status.guest`, `/run exec|ps|guest-out|killall`; `tv.py exec|ps`.
- CI: `macos-15`, Xcode 16.4, tvOS 18.5 SDK, deployment target 18.0; jobs
  `host-tests` (vfs_test + kernel_test), `fex-host-check` (FEXCore + rlkernel
  compile on x86-64), `build` (FEX step non-fatal → `FEX linked: true|false`
  in the release body; `librlfex_all.a` carries rlfex + rlkernel).
- Shared branch is `claude/rocket-league-apple-tv-zek0mi` until `main`
  exists (see CLAUDE.md "Shared branch" and DECISIONS.md).

## laptop

- **Request 004 (build-23): PARTIAL, paused** (2026-09-27 04:49–05:25 CEST).
  - Passed: step 0 (address space holds at 7.25 GB across 6 guests, `allocator: system`) and
    1b.
  - 1a and 1c hang: the forked child loops forever on an unaligned `stlurh` into a CoW page;
    `cow_fault()` claims every fault on a tracked page, so FEX's unaligned fix-up never runs.
  - Steps 2–5 are not run: the TV went to standby twice during hangs, so LAPTOP asked the user
    before waking it again.
  - Details: results/004-c3-shell/verdict.md.
- Request 003 (build-20): PARTIAL (C2 PASS; 4th-guest death, fixed in build-23).
- Request 002 (build-18): PARTIAL, C1 PASS; C2's SIGBUS was fixed in build-20.
- The TV is in standby. `tv.py launch` wakes it itself since build-20. If a debugger session
  dies mid-attach: `rltvos-jit signal --pid P` (SIGKILL via CoreDevice), then
  `tv.py launch --fresh`.
- Rootfs: busybox (dynamic PIE) replaced busybox-static; hello-static is `-static-pie`; manifest
  rebuilt. elf-audit: only compilers + python3.13 are non-PIE.
- Done earlier: request 001 PASS (JIT on tvOS 27 via the xcode-27 Cryptex DDI), setup S0–S8
  PASS (results/000-setup/verdict.md). Refs c1–d2 are in refs/; e1/e2 are in pre-release
  refs-laptop-1.
- S4 JIT: laptop/jit.sh --pid {pid}. DDI copy in ~/rltvos/ddi/tvos27, reinstalled automatically
  after a TV reboot. `rltvos-jit peek|gdb|regions|signal` = debugger diagnostics (read memory, packets, memory map, kill).
- atvloadly installs: 12 s for build-18 but 11 min for build-15, both with the DDI mounted
  (intermittent). INSTALL_TIMEOUT=1500.
- Issue 002-laptop-script-fixes.md: bugs in REPO's laptop scripts + the validated menu path.
- Services/paths:
  - Assets server: systemd --user `rltvos-assets` running REPO's laptop/assets_server.py on
    :8090. Manifest ~/rltvos/assets/manifest.jsonl.gz; rebuild with `--manifest-only --rebuild`
    after rootfs/prefix changes.
  - atvloadly MCP on :5533 with /share/ipa.
  - pyatv paired (~/.venvs/rltvos, ~/.pyatv.conf); gh and strace in ~/.local/bin.
- LAPTOP tools: laptop/install.sh + atvloadly.py (INSTALL_CMD via MCP), atv_pair.py,
  rootfs_exec.sh (bwrap: --gpu --nonet --rw, clean env, own hostname), and in results/000-setup:
  s6-rootfs-rootless.sh, s6-prefix.sh, s7-game.sh, s7-lowsettings.py, s8-game-traces.sh.
- Next: wait for REPO's next request (`handoff/wait.sh requests` running).
