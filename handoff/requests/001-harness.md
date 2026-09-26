# 001-harness — Phase A: install build-11, cycle, prove JIT (TXM pool), VA, mem on the TV

**Build:** `build-11` (release https://github.com/user99672223/RLtvOS/releases/tag/build-11;
`app.ipa` + `app.dSYM.zip`; `tv.py build --tag build-11` prints the sha256).
Unsigned; atvloadly signs it. Bundle id as built: `dev.rltvos.app` (the
signer appends the team suffix; `tv.py apps` resolves it). Any later green
`build-N` is also fine; say which one you used in the verdict. The release
body says whether FEX was linked (`FEX linked: true|false`); Phase A does not
need it. Read `handoff/issues/001-jit.md` first: the app implements the TXM
prepare-region protocol from your issue; your `laptop/jit.sh` is the other half.

**HOLD:** the user asked that the Apple TV is not touched until they say go.
Queue this request until then.

## What the app does (so you know what to expect)

- Shows a text console rendered through Metal: build tag, pid, tvOS version,
  `hw.machine`, MEM (phys_footprint / peak / available / limit), VA (largest
  contiguous PROT_NONE reservation + count of 1 GB steps), a JIT line, an
  ARENA line (legacy RWX probe), a FEX line, DBG (ptraced / cs_debugged /
  sigaltstack / crash report), CORE, log tail.
- Debug server on `TV_IP:7777` (LAN only): `GET /status /screenshot /log
  /mem /va /jit /crash /ping`, `POST /input /run /kill`, `DELETE /crash`.
- **JIT (jit26.c, the app half of issue 001):** at launch a background thread
  waits up to 60 s for `P_TRACED`, then calls the stubs
  `JIT26PrepareRegion(0, 128 MB)` (debugger-allocated region; the in-place
  form is off by default, see below) and `vm_remap`s an RW alias. Self-test
  attached (writes `mov w0,#42; ret` through RW, runs at RX, rewrites 43,
  runs), then `JIT26Detach()`, then the same test again (44/45). Everything
  is in `/status.jit`: `stage` (`waiting-for-debugger` → `preparing` →
  `remapping` → `testing` → `detaching` → `ready` | `failed`), `pool` (RX),
  `rw_alias`, `prepared_by` (`debugger` | `app-pool`), `attached_test`,
  `detached_test`, `detach_serviced`, `waited_s`, `unserviced_traps`,
  `error`. The console line reads `JIT ok pool 128 MB rx 0x.. rw 0x.. by
  debugger attached-test ok detached-test ok`.
- Without a debugger the `brk` traps are caught by the app (SIGTRAP guard):
  `stage: failed`, `unserviced_traps: 2`, and the app keeps running.
  `tv.py jit --prep` (= `/run jitprep`) restarts the wait at any time.
- Runtime knobs (persisted in Caches, no rebuild): `tv.py jitcfg
  jit_pool_mb=128 jit_wait_s=60 jit_in_place=0 jit_detach=1 jit_selftest=1
  jit_autostart=1`. `jit_in_place=1` maps the RX pool in the app and asks the
  debugger to prepare *that* address first (AetherPS4 found this form
  silently useless with StikDebug; try it once, as an experiment).
  `jit_detach=0` keeps the debugger attached (use if the detached test kills
  the app: `/status.last_jit_kill` shows `jit26 detached-test` after relaunch).
- `sysinfo.sigaltstack` in `/status` says whether `sigaltstack` (SDK-prohibited
  on tvOS) works at runtime.
- FEX (Phase B, only if the release says `FEX linked: true`): `tv.py fex
  selftest` runs seven bare x86-64 programs through FEXCore's JIT in the
  pool (add, loop, sse, call, mem, syscall, exit). Optional for this request;
  a result here is a Phase B result and welcome.

## Steps (run from the repo root on the laptop)

1. Config: fill `laptop/config.env` — `TV_IP`, `TV_DEVICE_ID`, `APP_BUNDLE_ID`,
   the install path (`INSTALL_CMD` = your `laptop/install.sh`), and the JIT
   helper: `JIT_CMD` (e.g. `laptop/jit.sh --launch {bundle_id}` with
   `JIT_LAUNCHES_APP=1`, or `laptop/jit.sh --pid {pid}`), `JIT_CMD_BACKGROUND=1`
   if the helper stays in the foreground while servicing requests.
2. `python3 tools/tv.py build --tag build-11` → downloads the IPA.
3. `python3 tools/tv.py install --tag build-11` (`--force` to reinstall).
   If the "Local Network" permission dialog appears on the TV at first
   launch, allow it with the remote (`atvremote --id ... select`).
4. `python3 tools/tv.py launch --fresh` → expect `"up": true`, `build-11`.
   (With `JIT_LAUNCHES_APP=1` skip this: step 6 launches.)
5. `python3 tools/tv.py status > status.json`, `python3 tools/tv.py va`,
   `python3 tools/tv.py mem` (before JIT).
6. JIT: `python3 tools/tv.py jit` → runs `JIT_CMD`, polls `/status.jit`
   until `ready`/`failed`, then re-runs the pool self-test. Expect
   `jit.ok = true`, `attached_test = true`, `detached_test = true`,
   `prepared_by = "debugger"`. Then `python3 tools/tv.py mem` (after: the
   prepared pool is resident — record the delta). If the app dies during
   the detached test: `tv.py launch`, `tv.py status` (`last_jit_kill`),
   `tv.py jitcfg jit_detach=0`, `tv.py launch --fresh`, `tv.py jit` again.
   If `PrepareRegion(0, len)` fails but the helper is attached, try
   `tv.py jitcfg jit_in_place=1` + relaunch once and report both outcomes.
7. `python3 tools/tv.py cycle --tag build-11` → writes
   `$OUT_DIR/cycles/cycle-<ts>/{shot.png,mem.json,status.json,log.txt,report.json}`.
8. Crash handler check: `python3 tools/tv.py run -- crashtest 0` (app
   crashes in 300 ms), then `python3 tools/tv.py launch` and
   `python3 tools/tv.py crash` → save the report text. Then
   `python3 tools/tv.py crash --clear`.
9. Optional (assets server running): `python3 tools/tv.py vfs mount`, then
   `vfs ls /`, `vfs cat /etc/hostname`, `vfs stats`. Note: the app still
   expects REPO's manifest layout (`/manifest.jsonl.gz`, `/f/<root>/<path>`);
   adapting it to your caddy layout is on REPO's list — skip if it 404s.
10. Optional, FEX linked: `python3 tools/tv.py fex selftest` after step 6;
    save the JSON. If it fails, `tv.py log --all` and `tv.py crash` after a
    relaunch are what REPO needs.
11. `python3 tools/tv.py result 001-harness --verdict PASS|FAIL --from
    $OUT_DIR/cycles/cycle-<ts> --note "<what the TV showed>"`, add
    `crash.txt`, the `va`/`mem` JSON, the `tv.py jit` output as `jit.json`
    (and `fex.json` if run), commit `[laptop] result 001-harness`,
    `git pull --rebase`, push.

## Capture

- `shot.png` (from cycle), `status.json`, `mem.json` (before/after JIT),
  `log.txt`, `crash.txt`, `report.json`, `jit.json`, and `verdict.md` with:
  tvOS version, `hw_machine`, `kern_osversion`, whether the Local Network
  prompt appeared, how install and JIT were done (exact commands), the
  helper's own log for the two `brk` requests, and anything odd on screen.

## Pass criteria

- PASS: the screenshot shows the console with the MEM line (non-zero
  phys_footprint and available), the VA line with numbers, and `JIT ok`
  with `attached-test ok` (detached-test may be `FAIL`/`not-detached` if
  you had to keep the debugger attached — say so).
- If the helper cannot attach or `PrepareRegion` never returns a region:
  verdict FAIL, still deliver everything above; `/status.jit` (`stage`,
  `error`, `unserviced_traps`, `ptraced`, `waited_s`) and the helper log go
  into `handoff/issues/003-jit-txm-brk-protocol.md`.
- Record: `phys_footprint` before/after, `waited_s`, `prepare_s`,
  `remap_s`, `prepared_by`, `detach_serviced`, `/status.sysinfo.sigaltstack`
  and `/va` verbatim; they set design parameters for Phase B/C.
