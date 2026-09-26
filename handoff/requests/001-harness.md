# 001-harness — Phase A: install build-5, cycle, prove JIT/VA/mem on the TV

**Build:** `build-5` (release https://github.com/user99672223/RLtvOS/releases/tag/build-5,
`app.ipa` sha256 `da77761d5bc5bffa6281eacb862fd0caf895efc0477d2435f1ef380075ca9096`,
`app.dSYM.zip` alongside). Unsigned; atvloadly signs it. Bundle id as built:
`dev.rltvos.app` (the signer may append a team suffix; `tv.py apps` resolves it).
Any later green `build-N` is also fine (it only adds to the harness); say
which one you used in the verdict. Read `handoff/issues/001-jit.md` first:
on tvOS 26+ JIT needs a debugger write per page, which `tv.py jit` does.

## What the app does (so you know what to expect)

- Shows a text console rendered through Metal (blue bar top, orange bar bottom,
  a green marker that moves every frame): build tag, pid, tvOS version,
  `hw.machine`, MEM (phys_footprint / peak / available / limit), VA (largest
  contiguous PROT_NONE reservation + count of 1 GB steps), JIT line, DBG line
  (ptraced / cs_debugged / crash report present), log tail.
- Debug server on `TV_IP:7777` (LAN only): `GET /status /screenshot /log
  /mem /va /jit /crash /ping`, `POST /input /run /kill`, `DELETE /crash`.
- JIT: at launch the app maps a 64 MB JIT arena (`/status.jit_arena`:
  base, size, prot rwx|rw, rwx_errno) and runs a map-only probe (nothing
  executes). `tv.py jit` attaches through debugserver, writes every 16 KB
  page of the arena back to itself (the TXM authorization), detaches, then
  `POST /run {"argv":["jittest","--trust"]}`: the app writes `mov w0,#42;
  ret` into an arena page, executes it, rewrites it to return 43, executes
  again → `JIT ok` on screen. `--madvise` / `--fresh` are experiments (see
  the issue); `--fresh` is expected to SIGKILL the app — run it last.
- `sysinfo.sigaltstack` in `/status` says whether `sigaltstack` (SDK-prohibited
  on tvOS) works at runtime.

## Steps (run from the repo root on the laptop)

1. Config: fill `laptop/config.env` — `TV_IP`, `TV_DEVICE_ID`, `APP_BUNDLE_ID`,
   and one install path (`INSTALL_CMD`, or `ATVLOADLY_MCP_INSTALL_TOOL` +
   `ATVLOADLY_MCP_INSTALL_ARGS` after `tools/tv.py mcp list`). Set `JIT_CMD` or
   `ATVLOADLY_MCP_JIT_TOOL` if a JIT method exists; leave empty otherwise.
2. `python3 tools/tv.py build --tag build-3` → downloads the IPA.
3. `python3 tools/tv.py install --tag build-3` (`--force` to reinstall).
   If the "Local Network" permission dialog appears on the TV at first
   launch, allow it with the remote (`atvremote --id ... select`).
4. `python3 tools/tv.py launch --fresh` → expect `"up": true` and a `build`
   field of `build-3`.
5. `python3 tools/tv.py status > status.json` and `python3 tools/tv.py va`,
   `python3 tools/tv.py mem`.
6. JIT (see `handoff/issues/001-jit.md`): get a debugserver for the TV
   without root, e.g. `pymobiledevice3 developer debugserver start-server
   --userspace --udid <UDID>`, put its address in `DEBUGSERVER_ADDR` (or the
   command in `DEBUGSERVER_CMD`), then:
   `python3 tools/tv.py mem` (before) → `python3 tools/tv.py jit` →
   `python3 tools/tv.py mem` (after; the authorized arena becomes resident).
   Expect `authorized_pages` = 4096 and `jittest.ok = true`. Then the
   experiments, each followed by `tv.py status` (or `tv.py launch` +
   `tv.py status` if the app died): `tv.py jit --page 1 --madvise`,
   and last `tv.py jit --fresh` (expected SIGKILL; `/status.last_jit_kill`
   after relaunch confirms it). If no debugserver route works, record the
   exact errors in the issue; the map-only `jit` object in `/status`
   (`rwx_errno`, `cs_debugged`, `ptraced`, `jit_arena.prot`) is still
   needed.
7. `python3 tools/tv.py cycle --tag build-3` → writes
   `$OUT_DIR/cycles/cycle-<ts>/{shot.png,mem.json,status.json,log.txt,report.json}`.
8. Crash handler check: `python3 tools/tv.py run -- crashtest 0` (app
   crashes in 300 ms), then `python3 tools/tv.py launch` and
   `python3 tools/tv.py crash` → save the report text. Then
   `python3 tools/tv.py crash --clear`.
9. `python3 tools/tv.py result 001-harness --verdict PASS|FAIL --from
   $OUT_DIR/cycles/cycle-<ts> --note "<what the TV showed>"`, add the crash
   report as `crash.txt` and the `va`/`mem` JSON, commit `[laptop] result
   001-harness`, `git pull --rebase`, push.

## Capture

- `shot.png` (from cycle), `status.json`, `mem.json`, `log.txt`, `crash.txt`,
  `report.json`, and `verdict.md` with: tvOS version, `hw_machine`,
  `kern_osversion`, whether the Local Network prompt appeared, how install
  and JIT were done (exact commands / MCP tools), and anything odd on screen.

## Pass criteria

- PASS: the screenshot shows the console with the MEM line (non-zero
  phys_footprint and available), the VA line with numbers, and `JIT ok`
  (after `tv.py jit`).
- If no debugserver route to the TV works yet: verdict FAIL, but still
  deliver everything above; the JIT line will read `JIT mapped-only ...`
  and `/status.jit_arena.prot` / `rwx_errno` tell us whether tvOS 27 lets
  an undebugged process map RWX at all. Update `handoff/issues/001-jit.md`
  with what was tried and the exact errors (this is the skeleton's biggest
  risk).
- Record the mem delta of authorization (`phys_footprint` before/after
  `tv.py jit`), the `--madvise` and `--fresh` outcomes, and
  `authorize_seconds` from the `tv.py jit` output.
- Report `/status.sysinfo.sigaltstack` and `/va` numbers verbatim; they set
  design parameters for Phase B/C.
