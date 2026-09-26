# DECISIONS — one dated line per deviation from the brief

- 2026-09-26 [repo] Shared branch: repo was empty and the REPO session's harness restricts pushes to `claude/rocket-league-apple-tv-zek0mi`; that branch is the integration branch until the user creates `main` from it. `handoff/wait.sh` follows `$HANDOFF_BRANCH` / the checked-out upstream / `main`; the build workflow triggers on `main` and `claude/**`.
- 2026-09-26 [repo] Xcode project is generated on the runner by XcodeGen from `app/project.yml` (no Xcode in the REPO session; a hand-written pbxproj is not maintainable blind).
- 2026-09-26 [repo] Debug server implemented as a C POSIX HTTP/1.1 server (`app/Sources/Native/httpd.c`) with routing in Swift; same LAN-only port 7777 contract as the brief. Reason: no local compiler for Swift, C is safer to write blind.
- 2026-09-26 [repo] `/log` returns JSON `{next, lines[]}` (host lines and guest strace-format lines share one sequence); `tools/tv.py log` writes the raw text to a file and prints JSON.
- 2026-09-26 [repo] `sigaltstack` is `__TVOS_PROHIBITED` in the tvOS SDK; the app resolves it with `dlsym` at runtime and reports the outcome in `/status.sysinfo.sigaltstack`. The FEX Darwin signal delegator will use an alt stack only if the box allows it.
- 2026-09-26 [repo] `tools/tv.py` speaks MCP (streamable HTTP JSON-RPC) directly to atvloadly (`tv.py mcp list|call`); install/JIT can be routed through `ATVLOADLY_MCP_*_TOOL` config keys instead of shell commands, so LAPTOP adapts via `config.env` without editing REPO files.
- 2026-09-26 [repo] Reference runs use `bubblewrap` (rootless) rather than `sudo chroot`; only the rootfs build step needs privileges. LAPTOP has no non-interactive sudo, so a `mmdebstrap --mode=unshare` path is being added to `10-rootfs.sh`.
