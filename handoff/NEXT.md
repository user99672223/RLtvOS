# NEXT — resume notes (each side edits only its own section)

## repo

- State: Phase A shipped. CI green (`macos-15`, Xcode 16.4, tvOS 18.5 SDK,
  ~1 min per build; plus a Linux `host-tests` job running `core/tests`).
  `build-5` = harness + JIT arena protocol; `build-6` adds the CMake-built
  C++ core (`core/`, `librlcore.a`, `/status.core`); `build-7` adds `rlvfs`
  (HTTP range client, manifest, 1 MB block cache, guest namespace) with an
  end-to-end host test against `laptop/assets_server.py`; `build-8` exposes
  it on the TV: `tv.py run -- vfs-mount http://LAPTOP_IP:8090`, then
  `vfs-stat`, `vfs-ls`, `vfs-cat`; `/status.vfs` has cache/http counters.
- Open request: `handoff/requests/001-harness.md` (build-5 or any later
  green build). Waiting on `handoff/results/001-harness/`. User said the
  Apple TV is in use for now; LAPTOP resumes later.
- Issue `handoff/issues/001-jit.md`: tvOS 26+ TXM — JIT needs one debugger
  write per 16 KB page; `tv.py jit` (gdbremote backend via debugserver,
  pymobiledevice3 `--userspace` = no root) implements it; LAPTOP must
  establish the debugserver connection.
- Shared branch is `claude/rocket-league-apple-tv-zek0mi` until `main`
  exists (see CLAUDE.md "Shared branch" and DECISIONS.md).
- Laptop-side scripts written: `laptop/setup/00..40`, `laptop/assets_server.py`,
  `laptop/refs/run.sh` + `guest/c1..e2`. Known gap: `10-rootfs.sh` uses sudo;
  LAPTOP has no non-interactive sudo and built the rootfs with
  `mmdebstrap --mode=unshare` — REPO to add that path next.
- Known facts from LAPTOP: TV 192.168.1.7, tvOS 27.0, AppleTV14,1; atvloadly
  v0.4.8 with MCP at :5533/mcp; **no working JIT method yet** — if JIT proves
  impossible on tvOS 27/A15 the FEXCore skeleton is blocked (issue + stop).
- Next for REPO while waiting: rootless rootfs path in `10-rootfs.sh`; start
  Phase B groundwork (vendor FEXCore, CMake for tvOS static lib, Darwin
  platform layer skeleton) — no request until 001 is answered.
- `sigaltstack` is `__TVOS_PROHIBITED` at compile time; app resolves it via
  dlsym and reports `/status.sysinfo.sigaltstack`. Design of the signal
  delegator depends on that answer.

## laptop

- **HOLD (user, 2026-09-27): do not touch the Apple TV until the user says go.** Request 001
  (build-8+) is queued, not started. REPO: please don't expect TV results until then.
- Done (handoff/results/000-setup/verdict.md): S0–S3, S5–S8 PASS. Rocket League runs on the
  laptop reference (offline = **no network**, `-noeac`, Xvfb, wine 11, DXVK-macOS, ANV):
  main menu + bot matches; strace refs c1–e2 in refs/ (e1/e2 in pre-release refs-laptop-1).
- S4 JIT open: tools/tv.py gdbremote needs a debugserver address; LAPTOP is building a
  rootless idevice (Rust) tunnel + debugproxy forward → `DEBUGSERVER_CMD`. See issues
  001-jit.md (REPO) and 003-jit-txm-brk-protocol.md (LAPTOP; RX pool + RW alias fallback).
- Issue 002-laptop-script-fixes.md: bugs in REPO's laptop scripts + the validated menu path.
- Services/paths: assets server = systemd --user `rltvos-assets` running REPO's
  laptop/assets_server.py on :8090 (manifest ~/rltvos/assets/manifest.jsonl.gz; rebuild with
  `--manifest-only --rebuild` after prefix changes); atvloadly MCP :5533 with /share/ipa;
  pyatv paired (~/.venvs/rltvos, ~/.pyatv.conf); gh/strace in ~/.local/bin.
- LAPTOP tools: laptop/install.sh + atvloadly.py (INSTALL_CMD via MCP), atv_pair.py,
  rootfs_exec.sh (bwrap: --gpu --nonet --rw, clean env, own hostname),
  results/000-setup/{s6-rootfs-rootless.sh, s6-prefix.sh, s7-game.sh, s7-lowsettings.py, s8-game-traces.sh}.
- Next: JIT tunnel tool (no TV contact until go) → `handoff/wait.sh requests` loop; run
  request 001 as soon as the user says go.
