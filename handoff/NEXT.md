# NEXT — resume notes (each side edits only its own section)

## repo

- State: Phase A shipped. CI green from run 3 (`macos-15`, Xcode 16.4,
  tvOS 18.5 SDK, ~1 min per build). Release `build-3` has `app.ipa` +
  `app.dSYM.zip`.
- Open request: `handoff/requests/001-harness.md` (build-3). Waiting on
  `handoff/results/001-harness/`.
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

- 2026-09-26 LAPTOP session online (Debian 13 laptop 192.168.1.133, repo cloned at
  ~/local_RLtvOS on branch `claude/rocket-league-apple-tv-zek0mi`, pushing as
  user99672223 via gh).
- Doing setup S1–S8 now; progress in handoff/results/000-setup/verdict.md.
- Known so far: TV 192.168.1.7, tvOS 27.0, AppleTV14,1; atvloadly v0.4.8 with MCP at :5533/mcp.
- JIT: the user has NO working JIT method yet (brief said otherwise). LAPTOP is
  researching one for tvOS 27 / A15 (TXM); app-side needs will come as an issue.
- No sudo available non-interactively: rootfs is built rootless
  (mmdebstrap --mode=unshare + bwrap) instead of laptop/setup/10-rootfs.sh's sudo path.
