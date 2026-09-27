# NEXT — resume notes (each side edits only its own section)

## repo

- State: Phase A answered by LAPTOP (result 001: PASS except JIT, TV
  numbers in PROGRESS.md). Phase B code compiles and links into the app
  since build-13 (`FEX built: true, FEX linked: true` in the release body);
  it needs the TXM pool to run, which needs the tvOS 27 DDI (LAPTOP + user). CI: `macos-15`, Xcode 16.4, tvOS 18.5 SDK,
  deployment target now 18.0; jobs `host-tests`, `fex-host-check` (vendored
  FEXCore compiles on x86-64 Linux — passes locally) and `build`. The
  FEX step (`core/fex` → `librlfex_all.a`) is allowed to fail: the app is
  then built without FEX (`RL_HAVE_FEX` off, console says "FEX not linked")
  and `fex-build.log` is in the `build-logs-N` artifact; the release body
  says `FEX linked: true|false`.
- Request 001 is answered (FAIL = JIT only). Next request 002 (no-JIT
  checks on build-14+): `tv.py va --probe2` (reservation limit experiments
  that fix the memory design), `tv.py fex status|init` with FEX linked
  (expect "JIT pool not ready", no crash), then the kernel loader dry-run
  once it lands. The JIT + FEX self-test re-run is its own request when
  the DDI exists.
- JIT: app half of LAPTOP's issue done in `app/Sources/Native/jit26.c`
  (`/status.jit`, `tv.py jit|jitcfg`, details in `handoff/issues/001-jit.md`).
  Debugger-allocated region is the default; in-place pool is an experiment.
- FEX: `third_party/FEX` @59f85d6 + Darwin patches (`RLTVOS-PATCHES.md`) incl.
  the RW/RX dual-mapping translation ported from AetherPS4's fork;
  `core/fex/src/{darwin_platform,rlfex}.cpp` (log/threads/config/host
  features/alloc hooks; bare-function runner; `/run fex-selftest`).
  First tvOS compile will surface Apple-only errors — fix from
  `fex-build.log`, re-push, repeat until `FEX linked: true`.
- Shared branch is `claude/rocket-league-apple-tv-zek0mi` until `main`
  exists (see CLAUDE.md "Shared branch" and DECISIONS.md).
- LAPTOP's issue 002 script fixes applied (rootless rootfs, `--bind`,
  `--gpu`, wine i386 dummy, `-ldxguid -luuid`, `KEY= # comment`).
- VFS layout question is closed: LAPTOP serves REPO's `assets_server.py`
  (manifest.jsonl.gz), mount/ls/cat PASS on the TV.
- To do next (REPO): `core/kernel` (fake kernel) C1/C2: ELF loader from the
  VFS, shared address space with VMAs (kernel-design §2 rewritten for the
  6 GB VA budget), fd table, syscall table with strace-format log,
  `/run exec ARGV...` on the TV; host unit test for the loader.
- `sigaltstack` is `__TVOS_PROHIBITED` at compile time; app resolves it via
  dlsym and reports `/status.sysinfo.sigaltstack`. Design of the signal
  delegator depends on that answer.

## laptop

- HOLD lifted by the user 2026-09-27 ~02:00. **Request 001 (build-11) ran: FAIL only on JIT**:
  harness, MEM/VA, crash handler, VFS all work on the TV (results/001-harness/verdict.md).
- **JIT blocker:** tvOS 27 needs the Cryptex1 developer disk image from **Xcode 27** (issue 003,
  last section). Asked the user for a Mac with Xcode 27 or an Xcode 27 .xip download. Then:
  add cryptexd install to laptop/jit, re-run 001.
- Done (handoff/results/000-setup/verdict.md): S0–S3, S5–S8 PASS. Rocket League runs on the
  laptop reference (offline = **no network**, `-noeac`, Xvfb, wine 11, DXVK-macOS, ANV):
  main menu + bot matches; strace refs c1–e2 in refs/ (e1/e2 in pre-release refs-laptop-1).
- S4 JIT: helper built (laptop/jit/ Rust + laptop/jit.sh; JIT_BACKEND=cmd, attach mode
  `jit.sh --pid {pid}`); tunnel proven on the TV; waiting on the tvOS 27 DDI (see above).
- Issue 002-laptop-script-fixes.md: bugs in REPO's laptop scripts + the validated menu path.
- Services/paths: assets server = systemd --user `rltvos-assets` running REPO's
  laptop/assets_server.py on :8090 (manifest ~/rltvos/assets/manifest.jsonl.gz; rebuild with
  `--manifest-only --rebuild` after prefix changes); atvloadly MCP :5533 with /share/ipa;
  pyatv paired (~/.venvs/rltvos, ~/.pyatv.conf); gh/strace in ~/.local/bin.
- LAPTOP tools: laptop/install.sh + atvloadly.py (INSTALL_CMD via MCP), atv_pair.py,
  rootfs_exec.sh (bwrap: --gpu --nonet --rw, clean env, own hostname),
  results/000-setup/{s6-rootfs-rootless.sh, s6-prefix.sh, s7-game.sh, s7-lowsettings.py, s8-game-traces.sh}.
- Next: get the tvOS 27 DDI (user) → cryptexd install in laptop/jit → re-run request 001.
  `handoff/wait.sh requests` keeps running for new requests.
