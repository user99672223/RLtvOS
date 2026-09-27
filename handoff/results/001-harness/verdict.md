# 001-harness — build-11 (+ build-15 for FEX) on the Apple TV

**Verdict: PASS** — the console shows MEM, VA and **`JIT ok … attached-test ok detached-test ok`**
(jit-rerun/shot-jit-ok.png). First run 02:05 CEST was FAIL (JIT only: no tvOS 27 developer
disk image). Re-run 02:22 after installing Xcode 27's **tvOS Cryptex1 DDI**, obtained on
GitHub Actions (`xcode-27` runner, .github/workflows/laptop-tvos-ddi.yml) and installed
through cryptexd by `laptop/jit` (`mount-ddi`). Bonus (step 10, build-15 with FEX linked):
**FEXCore JIT runs on the TV, fex selftest 3/7.**

Builds: **build-11** (f584db1, IPA sha256 c3c25017…) for steps 2–9; **build-15** (FEX linked,
IPA sha256 aabaec4c…) for step 10.

## JIT (step 6) — how it was done
1. DDI, once per TV boot: `rltvos-jit mount-ddi --ddi ~/rltvos/ddi/tvos27/published/Xcode_tvOS_DDI_Cryptex`
   (tvOS DDI build 27A9269, "tvOS Customer Developer Disk Image Cryptex", ProductClass 0xF4):
   TSS + cryptexd install in **5.8 s** → `com.apple.MobileAsset.DDI` 27.1.9269.0; RSD services
   60 → **80**, including `com.apple.internal.dt.remote.debugproxy` and
   `com.apple.coredevice.appservice` (jit-rerun/ddi-install.jsonl, jit-probe-after-ddi.json).
   `jit.sh` now does this automatically when the debug services are missing.
2. `tv.py launch --fresh` → `tv.py jit` → `JIT_CMD = laptop/jit.sh --pid {pid}` → helper
   (RemotePairing tunnel, debugproxy, universal.js loop). Helper log, verbatim
   (jit-rerun/helper-build11.log):
   ```
   {"event":"tunnel","t":0.194,"services":80}
   {"event":"attached","t":1.439,"pid":1055,"reply":"T11"}
   {"event":"prepared","t":5.815,"addr":"0x105fec000","len":134217728,"pages":8192,"allocated_by":"debugger","ms":4206,"regions":1}
   {"event":"detached","t":6.048,"reply":"OK","regions":1,"stops":2}
   ```
   i.e. the two brk requests were serviced: PrepareRegion(0, 128 MB) → `_M8000000,rx` +
   8192 page writes, then Detach → `D`.
3. `/status.jit` (jit-rerun/jit-build11.json): `stage ready`, `prepared_by debugger`, pool rx
   `0x105fec000`, rw_alias `0x10dfec000` (delta 128 MB), `attached_test true`,
   **`detached_test true`** (after the debugger left: `ptraced 0`, `cs_debugged 1`),
   `detach_serviced true`, `waited_s 2`, **`prepare_s 4.38`**, **`remap_s 0.014`**,
   `unserviced_traps 0`; `jittest --trust`: result1 42, result2 43, ok. `authorize_seconds 6.4`.
4. **Memory:** phys_footprint **31.8 MB → 167.8 MB** (+136 MB: the prepared 128 MB pool is
   resident), available 2066.2 → 1930.2 MB, limit~ 2098 MB (mem-before/after-jit.json).
5. **In-place experiment** (`jitcfg jit_in_place=1`, relaunch, jit): **also works on tvOS 27**.
   `prepared_by app-pool`, pool `0x1083ec000` = the app's own RX mapping, prepared by the
   debugger in 4.27 s, attached/detached tests ok (jit-build11-inplace.json,
   helper-build11-inplace.log). Unlike AetherPS4's report, the app can choose the pool address.
   Restored `jit_in_place=0`.
6. `tv.py cycle --tag build-11` with JIT: all steps ok in 13 s, `jit_stage ready`
   (jit-rerun/cycle-*).

## FEX (step 10, build-15, JIT ready)
`tv.py fex selftest` (jit-rerun/fex-selftest-build15.json, fex-log-build15.txt):
`rlfex bound to the jit26 pool`; init ok in 7.3 ms, FEX-59f85d6-rltvos, page_size 16384,
probe rw 0x1123ec000 / rx 0x10a3ec000, 16 MB code buffer from the pool. Tests: **add ok (24),
call ok (42), mem ok (42)**; loop FAIL (100, expected 5050), sse FAIL (13, expected 42),
syscall FAIL (0, expected 4242, syscalls 0), exit FAIL (0, expected 7). No faults, no crash,
every run `exit hlt` at the same rip 0x1039d0009, runs after the first take 0.01 ms.
LAPTOP's guess: all tests run at the same guest address, so FEX's block cache reuses the
previous test's translation; invalidate the code cache, or give each test its own address.
syscall/exit may also simply not be wired yet (0 syscalls).

## Numbers (verbatim)
- `/va`: `{"contig_errno":12,"highest":"0x7180000000","lowest":"0x10c800000","max_contiguous_gb":6,"step_limit_gb":1024,"stop_errno":12,"total_1gb_steps":7}`
- `/status.sysinfo.sigaltstack`: `ok`; hw_machine `AppleTV14,1`; kern_osversion `24J361`; tvOS 27.0.
- Legacy arena: `mmap(RWX)` succeeds without a debugger (`rwx_errno 0`), mapped only.
- No debugger: brk → `stage failed`, "PrepareRegion returned 0 (…unserviced traps=1, waited 60.1 s)",
  and the app lives on (SIGTRAP guard works; first run, status-after-jit-window.json).

## Other steps (first run, files in this directory)
- 3 install: laptop/install.sh (atvloadly MCP `install_app`, /share/ipa path) → `dev.rltvos.app.GBCWA7VWJ3`, ~5 s.
- 4/5 launch, status, va, mem: ok (status-before-jit.json, va.json, mem-before-jit.json).
- 7 cycle (no JIT): ok except JIT (report.json, shot.png, log.txt).
- 8 crash: `crashtest 0` → SIGSEGV addr 0x10, symbolized backtrace (crash.txt), cleared.
- 9 vfs: mount 528.6 ms, 70,386 entries / 48.19 GB; ls/cat/stat ok, 0 errors (vfs-*.json*).
- **No "Local Network" prompt** at any point (tv-screen-*.jpg).

## Odd things
- **Installs are slow while the tvOS 27 DDI is mounted:** build-15 took **11 min**; atvloadly's
  plumesign was idle between AFC upload (00:24:16) and install progress (00:35:28).
  First log line: `idevice::xpc::format Body length is 23584, but received bytes is 16374`.
  The larger RSD service list (80 services) is probably mis-read by plumesign's older idevice
  (jit-rerun/atvloadly-install-build15-timeline.txt). LAPTOP raised INSTALL_TIMEOUT to 1500 s.
- Log timestamps jump (981.1 → 20851.4 → 40095.8 while uptime is minutes).
