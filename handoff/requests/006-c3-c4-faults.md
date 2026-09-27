# Request 006 — build-27: C3/C4 again after the fault-classification fix, fault paths + address-space check (`crashtest`), cpubench on the TV

**Build:** `build-27` (release body `FEX built: true, FEX linked: true`; a later
green build with that line is fine — note the tag). build-26 was request 005's
kernel plus `/proc/<pid>/mem`; it never had a request.

**Priority:** issue 005 (`handoff/issues/005-memory-cap-experiment.md`, laptop
only, no TV) is the go/no-go experiment for E1/E2 and matters more than this
request — do it first if the TV is busy. Part D below (cpubench on the TV) is
the other decision input: run it at the start of the TV session.

What changed since build-25 (result 005):
- The fault guard classifies every SIGSEGV/SIGBUS from the arm64 exception
  syndrome (`__es.__esr`) instead of `si_code`, so a fork child's first
  copy-on-write write reaches the kernel again; alignment faults go straight to
  FEX's back-patcher; the guest gets Linux's signal numbers (SIGSEGV for
  permission/translation faults, SIGBUS for alignment). The `FAULT` log line
  now carries `kind=` (1 alignment, 2 permission, 3 translation).
- A thread that ends by a fault in JIT code or by a kick has its FEX thread
  destroyed: no more 272 MB of address space per such thread.
- `killall` records SIGKILL (exit 137, `term_signal 9`); `tv.py log --all`
  pages until the log is drained; `df` numbers are sane (1 GB tmpfs, 48 GB
  read-only image); new guest binary `/opt/rl/bin/crashtest`
  (`null-write|ro-write|handler|abort|loop`) for the fault paths.

Four parts, each with its own verdict line; if one part fails, still run the
others unless the app is dead.

## Prep (laptop)

1. `git pull`. Build `crashtest` into the rootfs: copy
   `laptop/refs/guest/src/crashtest.c` to `$ROOTFS_DIR/opt/rl/src/` and either
   re-run the customize step (`sh /opt/rl/customize.sh` inside the rootfs, the
   way `10-rootfs.sh` does; it is idempotent — cpubench is rebuilt too) or just
   `gcc -O2 -o /opt/rl/bin/crashtest /opt/rl/src/crashtest.c` in the chroot.
   Check with `rootfs_exec.sh -- /opt/rl/bin/crashtest handler` → `recovered:
   value=42 handled=1`, exit 0 (native). Rebuild the manifest
   (`--manifest-only --rebuild`), restart `rltvos-assets`.
2. `tv.py install --tag build-27`, `tv.py launch --fresh`, `tv.py jit`,
   `tv.py vfs mount`, then `tv.py va --probe2` → `va-fresh.json` and `tv.py mem`
   → `mem-fresh.json` (the fresh baseline 005 lacked).

## Part D — cpubench on the TV (issue 004, part B; first in the TV session)

D1. `tv.py exec --wait 900 -- /opt/rl/bin/cpubench` **twice**, back to back →
    `cpubench-tv-1.json/-stdout.txt`, `cpubench-tv-2.json/-stdout.txt` (run 1
    includes translation, run 2 is warm). Nothing else running, app in the
    foreground. The last stdout line is the JSON I parse. If a run takes longer
    than 15 min or the process dies: `tv.py log --all --out cpubench-log.txt`
    plus the exit info, and move on.

**Part D PASS**: two complete JSON lines. The TV/laptop ratios per test go into
PROGRESS.md (mine to interpret).

## Part A — C3 (005's A1–A3 again)

A1. `tv.py exec -- /usr/bin/sh -c 'echo one | tr o 0'` → `0ne`, exit 0, no
    `Bus error`, no `FAULT` line in the log → `a1.json/-stdout.txt`.
A2. `tv.py exec -- /usr/bin/sh -c 'sleep 1 & echo bg=$!; wait; echo waited'` →
    `bg=<pid>`, `waited`, wall time ≥ 1 s → `a2`.
A3. **C3 proper**: `tv.py exec --wait 120 -- /opt/rl/refs/c3.sh`. Expected
    output (the background line may come earlier):
    ```
    C3 start pid=<pid>
    3
    c
    b
    got-TERM
    after self-kill
    background job done
    false exit status: 1
    Name:	sh
    Umask:	0022
    State:	R (running)
    threads: counter=400000 (expect 400000) joined=60 (expect 60) usr1=1 tid=<pid>
    threads-test exit=0
    C3 done
    ```
    exit 0. Save `c3.json`, `c3-stdout.txt`, `tv.py log --all --out c3-log.txt`,
    `tv.py shot --out c3-shot.png`, `tv.py mem` → `c3-mem.json`.
A4. `tv.py va --probe2` → `va-a.json`: within 0.3 GB of `va-fresh.json` (no
    address space lost per finished process).

**Part A PASS**: A1–A3 as expected, A4 within 0.3 GB. **PARTIAL**: first
deviating step + the ~20 strace lines before it (for a `FAULT` line: the 40
lines before it, and the `kind=` value).

## Part B — C4 (005's B2–B4 again)

B2. `tv.py exec --wait 90 -- /usr/bin/sh -c 'Xvfb :1 -screen 0 320x240x24 -nolisten tcp -extension MIT-SHM >/tmp/xvfb1.log 2>&1 & for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do [ -S /tmp/.X11-unix/X1 ] && break; sleep 1; done; ls -la /tmp/.X11-unix; DISPLAY=:1 xdpyinfo | head -12; kill %1; wait; echo rc=$?'`
    → the socket node `X1`, xdpyinfo's `name of display: :1`, `vendor string:
    The X.Org Foundation`, `screen #0` … `dimensions: 320x240 pixels` →
    `b2.json/-stdout.txt`; `tv.py guest-file /tmp/xvfb1.log --out b2-xvfb1.log`;
    `tv.py log --all --out b2-log.txt`. (Xvfb needed ~15 s to load its
    libraries over the VFS in 005, hence the 20 s wait.)
B3. **C4 proper**: `tv.py exec --wait 180 -- /opt/rl/refs/c4.sh`. Expected
    output (timestamps vary):
    ```
    [hh:mm:ss] Xvfb up on :0 (pid N)
    name of display:    :0
    version number:    11.0
    vendor string:    The X.Org Foundation
    ...
    screen #0:
      dimensions:    1280x720 pixels (...)
    ...
    xwininfo: Window id: 0x... (the root window) (has no name)
      ...
         1 child:
         0x... "xeyes": ("xeyes" "XEyes")  400x300+100+100  +100+100
    [hh:mm:ss] snapshot /refs/out/c4.xwd
    C4 done
    ```
    exit 0. Save `c4.json`, `c4-stdout.txt`, `tv.py log --all --out c4-log.txt`,
    `tv.py guest-file /refs/out/xvfb.log --out c4-xvfb.log`,
    `tv.py guest-file /refs/out/c4.xwd --out c4.xwd` then `convert c4.xwd c4.png`,
    `tv.py shot --out c4-shot.png`, `tv.py mem` → `c4-mem.json` (once while Xvfb
    runs if convenient, once after).
B4. `tv.py ps` → `ps-b.json` (Xvfb, xeyes, xdotool, sh all `exited`,
    `mapped_bytes 0`), `tv.py va --probe2` → `va-b.json`, `tv.py mem` → `mem-b.json`.
B5. On a hang: `tv.py ps`, `tv.py log --all --out hang-log.txt`, `tv.py guest-file
    /refs/out/xvfb.log --out hang-xvfb.log`, then `tv.py ps --killall`; report
    which process/thread is stuck and its last strace line. On a `FAULT` line:
    the ~40 lines before it.

**Part B PASS**: B3 prints xdpyinfo's report for `:0` at 1280x720, xwininfo
lists the `xeyes` window, `c4.png` shows the eyes, `C4 done` with exit 0, app
alive. **PARTIAL**: first deviating step + the last ~20 strace lines before it.
**FAIL**: app crash → `crash-N.txt` + the log tail.

## Part C — fault paths and the address-space leak (`crashtest`)

Each step: `tv.py exec -- /opt/rl/bin/crashtest <mode>` → `c-<mode>.json/-stdout.txt`.

C1. `null-write` → `writing to NULL`, then the process dies: exit **139**
    (SIGSEGV); the log's `FAULT` line says `kind=3` (translation).
C2. `ro-write` → `writing to a read-only page 0x…`, exit **139**, `FAULT …
    kind=2` (permission). build-25 would have said 135 (Darwin's SIGBUS). If it
    prints `not reached` and exits 0 instead, the 4 KB page shared its 16 KB
    host page with a writable mapping — record it, that is a limit I want to see.
C3. `handler` → the `writing …` line, then **expected to die with 139 for
    now**: a guest SIGSEGV handler is not entered yet (a fault still kills the
    process; that is the next kernel item). Record what happens —
    `recovered: value=42 handled=1` / exit 0 would mean it already works.
C4. `abort` → `abort()`, exit **134** (SIGABRT through the guest's own signal
    path; no `FAULT` line).
C5. `loop` with `--wait 5` (the exec times out), then `tv.py ps --killall` →
    within a few seconds `dead`, `live_threads 0`, and now **exit code 137 /
    `term_signal 9`** in `tv.py ps` (005 A6's nit); log `kicked out of JIT code`.
C6. `tv.py va --probe2` → `va-c.json`: within 0.3 GB of `va-a.json` (005 lost
    272 MB per faulted or kicked thread; the five here must cost nothing).
C7. `tv.py ps` → `ps-c.json` (five crashtest processes `exited` with the codes
    above, `mapped_bytes 0`), `tv.py mem` → `mem-c.json`, `tv.py log --all --out
    c-log.txt` (the `FAULT` lines with `kind=`).
C8. `tv.py exec -- /usr/bin/sh -c 'df /tmp /; df /proc'` → `df.json`
    (tmpfs ~1 GB mostly free; `/` 48 GB full; proc 0).

**Part C PASS**: C1, C2, C4, C5, C6, C8 as stated (C3 is recorded, not judged).

## Files: `handoff/results/006-c3-c4-faults/`

`va-fresh.json`, `mem-fresh.json`; D: `cpubench-tv-1/2.json/-stdout.txt`;
A: `a1/a2.json/-stdout.txt`, `c3.json`, `c3-stdout.txt`, `c3-log.txt`,
`c3-shot.png`, `c3-mem.json`, `va-a.json`; B: `b2.json/-stdout.txt`,
`b2-xvfb1.log`, `b2-log.txt`, `c4.json`, `c4-stdout.txt`, `c4-log.txt`,
`c4-xvfb.log`, `c4.xwd`, `c4.png`, `c4-shot.png`, `c4-mem.json`, `ps-b.json`,
`va-b.json`, `mem-b.json`; C: `c-<mode>.json/-stdout.txt` ×5, `va-c.json`,
`ps-c.json`, `mem-c.json`, `c-log.txt`, `df.json`; `verdict.md` with one
verdict line per part; `hang-*`/`crash-*` if any.
