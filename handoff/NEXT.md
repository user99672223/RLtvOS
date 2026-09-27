# NEXT — resume notes (each side edits only its own section)

## repo

- State (2026-09-27): request 001 **PASS** including JIT (tvOS 27 DDI from
  Xcode 27 via the `xcode-27` Actions runner; details in results/001-harness
  and issue 003). FEXCore's JIT executes on the TV (build-15, 3/7 self-tests:
  the 4 "failures" ran the first test's stale translation — FEX's lookup
  cache is shared between threads; fixed by invalidating the code range,
  `InvalidateCodeBuffersCodeRange` + `InvalidateThreadCachedCodeRange`).
- Landed with the kernel commit: `core/kernel` (Phase C, C1/C2 scope) —
  `rlkernel_base` (ELF loader over `FileSource`, `AddressSpace` with per-4K
  shadow protections on 16K host pages + FEX invalidation hook, fd table,
  strace-format log; unit-tested by `kernel_test` in the host-tests job) and
  `rlkernel` (guest processes/threads on FEXCore, ~90 syscalls, synthetic
  /proc + /dev, rlvfs bridge, C API `rlkernel.h`). App: `/run exec [--dry-run]
  PATH...`, `ps`, `guest-out PID`, `killall`, absolute guest paths run
  directly, `KERN` console line, `/status.guest`. `tv.py exec|ps`.
- **Low addresses are impossible** (XNU `mach_loader.c`: arm64 64-bit
  binaries need a 4 GB hard page zero, else LOAD_BADMACHO). Guest
  executables must be PIE: `hello-static` is now `-static-pie`, `busybox`
  (dynamic) replaces `busybox-static`, `laptop/setup/45-elf-audit.sh` lists
  offenders; the loader refuses ET_EXEC images below 4 GB with ENOEXEC. Wine's
  0x7ffe0000 page is a C5 item (fault redirect, or one-constant Wine rebuild).
- Next request 002 (after the build with this commit is released): FEX
  self-test 7/7, `vaprobe2`, `45-elf-audit.sh`, rebuild `hello-static` as PIE
  (+ manifest rebuild), then **C1** `tv.py exec -- /opt/rl/bin/hello-static`
  and **C2** `tv.py exec -- /opt/rl/bin/hello-dyn` on the TV (dry-run first),
  screenshot with the KERN line, log with the strace lines, mem.
- Known gaps to close as the traces demand: exited processes are never
  reaped (their memory stays mapped), no SMC tracking (mprotect(+W) drops
  translations instead), one lock around every rlvfs call, no signal
  delivery yet (C3), no fork/execve/pipes yet (C3).
- CI: `macos-15`, Xcode 16.4, tvOS 18.5 SDK, deployment target 18.0; jobs
  `host-tests` (vfs_test + kernel_test), `fex-host-check` (FEXCore + rlkernel
  compile on x86-64), `build` (FEX step non-fatal → `FEX linked: true|false`
  in the release body; `librlfex_all.a` now also carries rlkernel).
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
