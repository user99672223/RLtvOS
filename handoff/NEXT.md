# NEXT — resume notes (each side edits only its own section)

## repo

- State: bootstrapping. First commit (CLAUDE.md, handoff/, PROGRESS.md)
  done; Phase A (workflow, tools/tv.py, in-app debug server) in progress.
- Shared branch is `claude/rocket-league-apple-tv-zek0mi` until `main`
  exists (see CLAUDE.md "Shared branch" and DECISIONS.md).
- Next: push Phase A, wait for release `build-<N>`, write
  `handoff/requests/001-harness.md`, then `handoff/wait.sh results` in the
  background while writing `laptop/setup/*.sh` and `laptop/refs/*.sh`.
- Open request: none yet.

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
