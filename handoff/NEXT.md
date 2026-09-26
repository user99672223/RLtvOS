# NEXT — resume notes (each side edits only its own section)

## repo

- State: Phase A shipped; Phase B code written, awaiting its first tvOS
  compile (build-11 on push). CI: `macos-15`, Xcode 16.4, tvOS 18.5 SDK,
  deployment target now 18.0; jobs `host-tests`, `fex-host-check` (vendored
  FEXCore compiles on x86-64 Linux — passes locally) and `build`. The
  FEX step (`core/fex` → `librlfex_all.a`) is allowed to fail: the app is
  then built without FEX (`RL_HAVE_FEX` off, console says "FEX not linked")
  and `fex-build.log` is in the `build-logs-N` artifact; the release body
  says `FEX linked: true|false`.
- Open request: `handoff/requests/001-harness.md` rewritten for the TXM
  flow (build-11 or later). Waiting on `handoff/results/001-harness/`;
  **user HOLD on the TV** stands (see ## laptop).
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
- To do next (REPO, no TV needed): rlvfs support for LAPTOP's caddy layout
  (`/manifest/index.json` + per-root JSON, files at `/<root>/<path>`);
  Phase C design → syscall table for C1 (`docs/kernel-design.md`).
- `sigaltstack` is `__TVOS_PROHIBITED` at compile time; app resolves it via
  dlsym and reports `/status.sysinfo.sigaltstack`. Design of the signal
  delegator depends on that answer.

## laptop

- **HOLD (user, 2026-09-27): do not touch the Apple TV until the user says go.** Request 001
  (build-8+) is queued, not started. REPO: please don't expect TV results until then.
- Done (handoff/results/000-setup/verdict.md): S0–S3, S5–S8 PASS. Rocket League runs on the
  laptop reference (offline = **no network**, `-noeac`, Xvfb, wine 11, DXVK-macOS, ANV):
  main menu + bot matches; strace refs c1–e2 in refs/ (e1/e2 in pre-release refs-laptop-1).
- S4 JIT: helper built (laptop/jit/ Rust + laptop/jit.sh; JIT_BACKEND=cmd, attach mode
  `jit.sh --pid {pid}`), unit-tested, **not yet run on the TV** (HOLD). First TV step when
  allowed: `laptop/jit.sh --probe` (tunnel + debug services), then request 001.
- Issue 002-laptop-script-fixes.md: bugs in REPO's laptop scripts + the validated menu path.
- Services/paths: assets server = systemd --user `rltvos-assets` running REPO's
  laptop/assets_server.py on :8090 (manifest ~/rltvos/assets/manifest.jsonl.gz; rebuild with
  `--manifest-only --rebuild` after prefix changes); atvloadly MCP :5533 with /share/ipa;
  pyatv paired (~/.venvs/rltvos, ~/.pyatv.conf); gh/strace in ~/.local/bin.
- LAPTOP tools: laptop/install.sh + atvloadly.py (INSTALL_CMD via MCP), atv_pair.py,
  rootfs_exec.sh (bwrap: --gpu --nonet --rw, clean env, own hostname),
  results/000-setup/{s6-rootfs-rootless.sh, s6-prefix.sh, s7-game.sh, s7-lowsettings.py, s8-game-traces.sh}.
- Next: `handoff/wait.sh requests` loop is running; request 001 runs as soon as the user says go.
