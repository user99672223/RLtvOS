# 001-harness — Phase A: install build-3, cycle, prove JIT/VA/mem on the TV

**Build:** `build-3` (release https://github.com/user99672223/RLtvOS/releases/tag/build-3,
`app.ipa` sha256 `4ad960691b8984440196d3f0dbbfb04170909a7b80cc8c8c9836e534966be3af`,
`app.dSYM.zip` alongside). Unsigned; atvloadly signs it. Bundle id as built:
`dev.rltvos.app` (the signer may append a team suffix; `tv.py apps` resolves it).

## What the app does (so you know what to expect)

- Shows a text console rendered through Metal (blue bar top, orange bar bottom,
  a green marker that moves every frame): build tag, pid, tvOS version,
  `hw.machine`, MEM (phys_footprint / peak / available / limit), VA (largest
  contiguous PROT_NONE reservation + count of 1 GB steps), JIT line, DBG line
  (ptraced / cs_debugged / crash report present), log tail.
- Debug server on `TV_IP:7777` (LAN only): `GET /status /screenshot /log
  /mem /va /jit /crash /ping`, `POST /input /run /kill`, `DELETE /crash`.
- The JIT test runs once at launch. It maps an RWX page, writes `mov w0,#42;
  ret`, executes it, rewrites the page to return 43, executes again. It only
  executes when the process is ptraced or has CS_DEBUGGED; otherwise it
  reports `exec-skipped` with `rwx_errno`. `POST /run {"argv":["jittest"]}`
  re-runs it (tv.py `jit` does that after JIT_CMD/MCP).
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
6. JIT: if a method exists, `python3 tools/tv.py jit` (runs the method, then
   re-runs the in-app test and returns `jittest`). If none exists yet, skip;
   the `/status` `jit` object (with `rwx_errno`, `cs_debugged`, `ptraced`)
   is the evidence we need either way.
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
  phys_footprint and available), the VA line with numbers, and `JIT ok`.
- If no JIT method exists yet: verdict FAIL, but still deliver everything
  above; the JIT line will read `JIT FAIL {... "stage":"exec-skipped" ...}`
  and `/status.jit.rwx_errno` tells us whether tvOS 27 even lets an
  undebugged process map RWX. Open/keep `handoff/issues/001-jit.md` with the
  JIT research status (this is the skeleton's biggest risk).
- Report `/status.sysinfo.sigaltstack` and `/va` numbers verbatim; they set
  design parameters for Phase B/C.
