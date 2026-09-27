# NEXT — resume notes (each side edits only its own section)

## repo

- **State (2026-09-27):** A, B, C1, C2, **C3 PASS** (build-27, result 006);
  C4 **PARTIAL**: Xvfb starts (xkbcomp via fork), sockets and xdpyinfo work,
  `xeyes`/`xdotool` hung because edge-triggered epoll entries (Xorg's clients)
  compared masks between scans and missed a drain-then-refill. **build-28**:
  an EPOLLET edge = the file's wait queue woken since the entry's last scan
  (`WaitQueue::wakes()` counter; kernel_test regression), translation faults
  are offered to `cow_fault` too (a child's first write to a page the parent
  never touched has no translation entry: result 006 C2 `kind=3`), statfs by
  path (`/` = the image, `/proc` no blocks), log ring 32 K lines, `tv.py
  launch` retries once after "launch_app is not supported", `crashtest
  fork-untouched`. **Open request: 007** (build-28): C4 B2 (`kill $!`) + B3,
  crashtest fork-untouched/ro-write/handler, df.
- crashtest on the TV (result 006): null-write 139 kind=3, ro-write 139 but
  kind=3 (untouched page: translation fault at the MMU even though the VMA
  exists — the guest `si_code` ACCERR/MAPERR must come from the VMA table, not
  the syndrome, when task 9 delivers SIGSEGV), handler not entered (task 9),
  abort 134, loop kicked → 137/term_signal 9, no address-space loss per fault.
- **Feasibility hold (issue 004 → 005):** the native game is 4.1–4.3 GB of
  private dirty anonymous memory + ~1.1 GB GPU buffers at 720p low; the TV
  kills at ~2.1 GB footprint (compressed and Metal memory count). Only
  file-backed guest memory (MAP_SHARED Caches file = external memory, outside
  the footprint) could fit it; issue 005 asks the laptop for the cgroup-cap +
  swap experiment (1–3 GB caps, fps, swap-in rate). **D1–E2 on hold until the
  user decides on that result**; C3–C5 verification of built code continues.
  If the user continues: file-backed guest anonymous memory moves to the front
  of the plan (before D1), plus `dxvk.maxChunkSize`, JIT cache cap, no audio.
- Next kernel item (task 9, build-28): guest fault / async signal delivery
  while a thread is in JIT code — in the guard, spill SRA GPRs/FPRs from the
  ucontext into CpuState via `SignalDelegatorConfig`, `rip =
  RestoreRIPFromHostPC`, EFLAGS via `ReconstructCompactedEFLAGS`, build the
  rt_sigframe with `deliver_signals` (SIGSEGV + SEGV_ACCERR/MAPERR from the
  fault kind), resume at `AbsoluteLoopTopAddressFillSRA` with x1 = 0; only
  when `DeferredSignalRefCount == 0`. Needed by Wine (SEH, SIGUSR1 suspend)
  and by `crashtest handler`.
- C5 prep from LAPTOP's `c5.summary.txt` (results/005-c3-c4): 104 syscalls,
  none ENOSYS natively; heavy: epoll_pwait2, getxattr (ENOTSUP ok), readlink,
  setitimer, clone3 (ENOSYS → clone), rseq. wine execs wine-preloader (ET_EXEC
  below 4 GB → ENOEXEC → falls back to the binary; verify). KUSER_SHARED_DATA at
  0x7ffe0000 is unmappable (4 GB page zero): plan A = fault-redirect with
  x86 load/store emulation of a phantom page (Wine's unix side reads it too;
  `anon_mmap_fixed` must "succeed"), plan B = Wine rebuild; decide at C5.
- Landed for C3 (build-21+): fork/vfork/clone (threads) with copy-on-write
  snapshots, execve (+ `#!`, CLOEXEC, fresh address space, CPU-state reset in
  place), wait4/SIGCHLD/zombies, pipe2, a real futex, guest signals (x86-64
  rt_sigframe, SA_RESTART/EINTR, default actions), interruptible sleeps,
  process reaping, `/proc/<pid>/mem` (ProcMemFile).
- Landed for C4 (build-25+): the writable overlay (`overlay.h`: tmpfs upper
  layer, whiteouts, copy-up, opaque /tmp /var/tmp /run /dev/shm; the whole
  fs syscall family, memfd_create, sendfile), record locks (`locks.h`), AF_UNIX
  sockets (`socket.h`), poll/ppoll/select/pselect6/epoll/eventfd (`poll.h`),
  setitimer/alarm (`timers.cpp`), MAP_SHARED upper files as real shared pages
  (SharedStore + vm_remap alias), `/guest-file` + `/guest-ls` debug routes
  (`tv.py guest-file|guest-put|guest-ls`). Guest scripts source `_lib.sh`
  relative to `$0`.
- Known gaps: signals reach a thread running JIT code only at its next
  syscall and a guest fault kills the process instead of entering its handler
  (task 9); no SMC tracking; one lock around every rlvfs call; MAP_SHARED at a
  misaligned offset falls back to a write-back copy; FIFO opens do not block
  for the other end; EPOLLET reports rising edges only; no AF_INET; itimer
  VIRTUAL/PROF never fire; a fault inside FEX's own runtime (not JIT code)
  still leaks the FEX thread object; 4 KB guest protections finer than the
  16 KB host page are the union (a read-only 4 KB page next to a writable one
  is writable).
- Facts: guest executables must be PIE (4 GB hard page zero); VA budget
  ~7.25–7.5 GB fresh; `/status.guest`, `/run exec|ps|guest-out|killall`;
  `tv.py exec|ps|guest-file|guest-put|guest-ls`.
- CI: `macos-15`, Xcode 16.4, tvOS 18.5 SDK, deployment target 18.0; jobs
  `host-tests` (vfs_test + kernel_test), `fex-host-check` (FEXCore + rlkernel
  compile on x86-64), `build` (FEX step non-fatal → `FEX linked: true|false`
  in the release body; `librlfex_all.a` carries rlfex + rlkernel). Pushes that
  touch only `handoff/`, `laptop/`, `tools/` do not build (paths-ignore), so
  build numbers skip.
- Local checks before every push: `cmake --build build/host && build/host/kernel/kernel_test`
  and `cmake --build build/fex-host --target rlkernel` (rlfex.cpp itself is
  Apple-only: CI is its first compile).
- Shared branch is `claude/rocket-league-apple-tv-zek0mi` until `main`
  exists (see CLAUDE.md "Shared branch" and DECISIONS.md).


## laptop

- **Request 006 (build-27)**: D cpubench PASS, A (C3) PASS; B (C4) PARTIAL and C (faults) PARTIAL.
  - cpubench: FEX on the A15 ≈ native laptop, total 0.85×; fp_scalar 1.28×, branchy 1.45×.
  - C4: Xvfb and xdpyinfo work in the guest; xeyes and xdotool hang because `epoll_wait` loses
    level-triggered readiness.
  - Faults: C2 logs `kind=3` (translation) and `df` shows `/` and `/proc` swapped.
  - Details: results/006-c3-c4-faults/verdict.md.
- Next: **issue 005** (memory-cap experiment, laptop only; REPO's go/no-go for E1/E2).
- **Request 005 (build-25): Part A PARTIAL, Part B PARTIAL** (2026-09-27 05:57–06:05 CEST).
  - Every *fork* child dies on its first stack write: the CoW write-protect fault arrives as
    SIGBUS si_code 1 and is misrouted to FEX's unaligned fix-up. vfork children are fine.
  - Passed: threads-test, signal exits, kick, B1 (writable layer + guest-file routes).
  - Xvfb (top-level): sockets and epoll OK; it dies at keyboard init because the xkbcomp fork
    dies.
  - Each thread that ends by FAULT or kick leaks a 272 MB FEX lookup cache (address space
    7.5 → 0.75 GB).
  - Details: results/005-c3-c4/verdict.md.
- **Issue 004 (laptop part) done**: native Rocket League footprint at the main menu is 4.2 GB Pss
  (97 % private anonymous in the game) + ~1.1 GB GPU buffers, ~4.4 GB in a bot match, 30 fps.
  cpubench native baseline done; the TV run comes with request 006. See
  results/issue-004/notes.md.
- Request 004 (build-23): PARTIAL (VA budget and 1b PASS; the fork hang, fixed in build-25);
  addendum with the 1c evidence.
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
