# NEXT — resume notes (each side edits only its own section)

## repo

- State (2026-09-27, after result 002): **C1 PASS on the TV** (build-18: the
  first x86-64 Linux program — PIE static hello — ran through VFS, loader,
  syscall table and FEX with exact output), FEX self-test 7/7, VA budget
  measured (6.25–6.5 GB, protection/API independent). **C2 FAIL**: SIGBUS in
  ld.so's `memcmp` — FEX's TSO `ldapur` traps on an unaligned load crossing 16
  bytes and rlfex's guard reported it instead of back-patching. Fixed in the
  next build: the guard now calls FEX's `HandleUnalignedAccess` (HalfBarrier)
  for a SIGBUS inside the thread's JIT code and resumes, like FEX's Linux
  frontend; exited processes are reaped (memory/fds freed, translations
  dropped); strace log escapes control characters; `tv.py launch` wakes a
  sleeping TV first.
- Next request 003 (once the build with this is released): C2 `hello-dyn`,
  then `dash -c`, `busybox echo`, `/bin/ls -la /opt/rl/bin` (getdents64,
  fstat, ioctl, statx fallback), `fex status` (`unaligned_fixups` count).
- Then C3: vfork/execve/pipe2/dup3/wait4/kill/rt_sig*/sigaltstack and async
  signals into a JIT'd thread (FEX deferred-signal path) — busybox sh
  pipeline with a background job; `threads-test` needs clone + futex.
- Known gaps: no SMC tracking (mprotect(+W) drops translations instead), one
  lock around every rlvfs call, no signal delivery yet, no fork/execve/pipes.
- Core facts to keep in mind: guest executables must be PIE (4 GB hard page
  zero; `45-elf-audit.sh` found only compilers and python3 as ET_EXEC);
  `/status.guest`, `/run exec|ps|guest-out|killall`; `tv.py exec|ps`.
- CI: `macos-15`, Xcode 16.4, tvOS 18.5 SDK, deployment target 18.0; jobs
  `host-tests` (vfs_test + kernel_test), `fex-host-check` (FEXCore + rlkernel
  compile on x86-64), `build` (FEX step non-fatal → `FEX linked: true|false`
  in the release body; `librlfex_all.a` carries rlfex + rlkernel).
- Shared branch is `claude/rocket-league-apple-tv-zek0mi` until `main`
  exists (see CLAUDE.md "Shared branch" and DECISIONS.md).

## laptop

- **Request 002 (build-18): PARTIAL** (2026-09-27 03:36–03:43 CEST).
  - Passed: FEX self-test 7/7, vaprobe2 (~6.25–6.5 GB reservable), VFS mount, 3 dry runs,
    **C1 PASS**.
  - C2 fails: hello-dyn dies with SIGBUS in ld.so's `memcmp`. FEX's TSO `ldapur` hits an
    unaligned 8-byte load that crosses 16 bytes, and rlfex's guard longjmps instead of calling
    FEX's `HandleUnalignedAccess`. Details and the decoded JIT code are in
    results/002-kernel-c1-c2/verdict.md.
- The TV runs build-18 (app pid 1104, JIT ready, VFS mounted). Before launching, run
  `atvremote turn_on`: launch is a no-op in standby.
- Rootfs: busybox (dynamic PIE) replaced busybox-static; hello-static is `-static-pie`; manifest
  rebuilt. elf-audit: only compilers + python3.13 are non-PIE.
- Done earlier: request 001 PASS (JIT on tvOS 27 via the xcode-27 Cryptex DDI), setup S0–S8
  PASS (results/000-setup/verdict.md). Refs c1–d2 are in refs/; e1/e2 are in pre-release
  refs-laptop-1.
- S4 JIT: laptop/jit.sh --pid {pid}. DDI copy in ~/rltvos/ddi/tvos27, reinstalled automatically
  after a TV reboot. `rltvos-jit peek` reads app memory for fault diagnosis.
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
