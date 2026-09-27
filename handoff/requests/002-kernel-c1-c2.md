# Request 002 — fake kernel on the TV: FEX self-test 7/7, C1 (static hello), C2 (glibc hello)

**Build:** `build-18` (this branch; the release body must say `FEX built: true,
FEX linked: true`). A later green build with the same line is fine too — note
the tag you used.

Context: request 001 is PASS incl. JIT (thanks — the DDI route is logged in
DECISIONS). build-15's FEX self-test 3/7 was a REPO bug: FEX's lookup cache is
shared between threads and every test's code page came back at the same
address, so tests 2–7 ran test 1's translation (rax = rdi+rsi+rdx every time).
The code range is invalidated before each run now. This build also carries the
first fake kernel (`core/kernel`): ELF loader over the VFS, address space,
fd table, ~90 syscalls with an strace-format log, synthetic /proc and /dev.

One hard fact learned from XNU's loader (`bsd/kern/mach_loader.c`): an arm64
64-bit process cannot map anything below 4 GB (hard page zero, no opt-out).
Every guest executable must therefore be PIE — `hello-static` is now built
`-static-pie` and `busybox` (dynamic, PIE) replaces `busybox-static`.

## Prep on the laptop (once)

1. `git pull`. Refresh the rootfs so the new binaries exist:
   `laptop/setup/10-rootfs.sh` (its refresh path: re-installs the package set —
   `busybox` replaces `busybox-static` — and re-runs `rootfs-customize.sh`,
   which now builds `hello-static` as `-static-pie`). If you prefer to do it
   by hand inside the rootfs: `apt-get install -y busybox` (removes
   busybox-static) and `sh /opt/rl/customize.sh`.
   Check: `file $ROOTFS_DIR/opt/rl/bin/hello-static` says "pie executable" or
   "shared object" (not "executable"); `file $ROOTFS_DIR/bin/busybox` says
   "pie executable, ... dynamically linked".
2. `laptop/setup/45-elf-audit.sh > elf-audit.txt` (read-only, python3 only):
   lists every non-PIE x86-64 executable in the rootfs with its load address.
   Save it in the result dir. Anything we need for C3–E2 in that list (wine
   loader, wineserver, Xvfb, xdotool, dash, coreutils) is a blocker to report.
3. Rebuild the manifest so the TV sees the new files:
   `laptop/assets_server.py --manifest-only --rebuild` (your systemd unit's
   manifest path), then restart `rltvos-assets`.
4. `tv.py install --tag build-18` (slow while the DDI is mounted — known),
   `tv.py launch --fresh`, `tv.py jit` → `jit_stage ready`.

## Steps on the TV (all from the laptop; save every JSON reply)

1. `tv.py fex selftest` → expect **7/7 ok** (`"ok": true`). Save
   `fex-selftest.json`; on any failure also `tv.py log --all --out fex-log.txt`.
2. `tv.py va --probe2` → save `vaprobe2.json` (reservation-limit experiments;
   a few seconds; the numbers fix the memory design).
3. `tv.py vfs mount` → ok (attaches the guest filesystem to the kernel).
4. Loader dry runs (nothing executes):
   - `tv.py exec --dry-run -- /opt/rl/bin/hello-static` → `dry-hello-static.json`
     (expect `ok:true`, `layout.main.dyn:true`, no interpreter, `maps` with the
     image, `[heap]`, `[stack]`).
   - `tv.py exec --dry-run -- /opt/rl/bin/hello-dyn` → `dry-hello-dyn.json`
     (expect `ok:true`, interp `/lib64/ld-linux-x86-64.so.2` mapped).
   - `tv.py exec --dry-run -- /usr/bin/sh` → `dry-sh.json`.
   An `ENOEXEC ... relink as PIE` error here means step 1 of the prep did not
   land (old binary in the manifest).
5. **C1** — `tv.py exec -- /opt/rl/bin/hello-static` (tv.py waits for exit and
   prints the guest output). Expected output, exit code 0:
   ```
   hello from x86-64 static
   333833500
   ```
   Save `c1.json`, then `tv.py log --all --out c1-log.txt` (the strace lines,
   e.g. `[1] write(1, "hello from x86-64 static\n", 25) = 25` …
   `[1] exit_group(0) = 0`), `tv.py shot --out c1-shot.png` (the console's
   `KERN` line shows the process with `exit=0`), `tv.py mem` → `c1-mem.json`.
6. **C2** — `tv.py exec -- /opt/rl/bin/hello-dyn`. Expected (exit 0):
   ```
   hello from x86-64 glibc (argc=1)
   uname: Linux 6.1.0-rltvos #1 SMP PREEMPT_DYNAMIC RLtvOS x86_64
   monotonic: <seconds>.<ns>
   pid=2 ppid=1 uid=1000 cwd=/
   exe=/opt/rl/bin/hello-dyn
   HOME=/home/user
   malloc 64MB ok 63
   ```
   (pid is 2 if C1 ran first in this app instance.) Save `c2.json`,
   `c2-log.txt` (`tv.py log --all --out ...` — the whole ld.so path in strace
   format), `c2-shot.png`, `c2-mem.json`.
7. Bonus, only if 5 and 6 passed: `tv.py exec -- /usr/bin/sh -c 'echo shell ok; echo $0 $$'`
   and `tv.py exec -- /bin/busybox echo busybox ok`. No fork/pipes exist yet,
   so subshells and pipelines are expected to fail — report the first wrong or
   `ENOSYS` syscall line from the log for each.
8. `tv.py ps` → `ps.json` (process table: states, exit codes, syscall counts).
   Diagnostic if step 5 fails in stage `fex` or with a FAULT right after
   step 1 succeeded: step 1 creates FEX's own context for the bare runner and
   the kernel creates a second one. Relaunch the app (`tv.py launch --fresh`,
   `tv.py jit`, `tv.py vfs mount`) and run step 5 first, *without* step 1;
   report both outcomes. (Two contexts in one process is untested on the TV.)
9. If the app dies at any step: `tv.py launch`, `tv.py crash --out crash-N.txt`,
   `tv.py jit`, and continue with the next step. A guest fault shows up as a
   `kernel: pid N tid M FAULT signal=… pc=… addr=… (guest rip=…)` log line
   with the app alive — include the ~40 log lines before it.

## Pass criteria

- **PASS**: step 1 is 7/7; step 5 prints exactly the two lines with exit 0;
  step 6 prints its lines with exit 0 (pid/monotonic values free); the app is
  alive afterwards; the screenshot shows the `KERN` line.
- **PARTIAL**: say which step is the first to deviate and quote the last ~20
  strace lines before the deviation (that is the next task on my side).
- **FAIL**: app crash → `crash-N.txt` + the log tail.

## Files: `handoff/results/002-kernel-c1-c2/`

`verdict.md` (PASS/FAIL + what the TV showed), `elf-audit.txt`,
`fex-selftest.json`, `vaprobe2.json`, `dry-*.json`, `c1.json c1-log.txt
c1-shot.png c1-mem.json`, `c2.json c2-log.txt c2-shot.png c2-mem.json`,
`ps.json`, bonus replies, `crash-*.txt` if any.
