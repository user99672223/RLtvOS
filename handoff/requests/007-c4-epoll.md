# Request 007 — build-28: C4 again (edge-triggered epoll fixed), fork of untouched pages, df

**Build:** `build-28` (release body `FEX built: true, FEX linked: true`; a later
green build with that line is fine — note the tag).

What changed since build-27 (result 006):
- `EPOLLET` entries report when the file's wait queue was woken since the
  entry's last scan (Linux's ready-list behaviour), not when the poll mask
  changed between two scans — Xorg registers every client edge-triggered and
  drains it to EAGAIN before re-entering `epoll_wait`, so xeyes's and xdotool's
  requests were never reported. Regression test in `kernel_test`.
- The copy-on-write hook also takes translation faults: a forked child's first
  write to a page its parent never touched has no translation entry yet
  (your C2 analysis); the page tables decide, alignment faults stay FEX's.
- `statfs` picks the filesystem by path (`/` = the 48 GB image, `/tmp`,
  `/var/tmp`, `/run`, `/dev/shm` = tmpfs 1 GB, `/proc` and `/sys` no blocks).
- The log ring holds 32 K lines; `tv.py launch` retries once when pyatv says
  `launch_app is not supported` right after the TV wakes; new
  `crashtest fork-untouched`.

Two parts, one verdict line each.

## Prep (laptop)

1. `git pull`. Rebuild `crashtest` into the rootfs (new mode; same procedure as
   006: copy `laptop/refs/guest/src/crashtest.c` to `$ROOTFS_DIR/opt/rl/src/`,
   `gcc -O2 -o /opt/rl/bin/crashtest /opt/rl/src/crashtest.c` in the chroot or
   re-run `customize.sh`). Native check: `rootfs_exec.sh -- /opt/rl/bin/crashtest
   fork-untouched` → `child: exit 0; parent sees m[0]=0 m[last]=0 (expect 0 0)`,
   exit 0. Rebuild the manifest (`--manifest-only --rebuild`), restart
   `rltvos-assets`.
2. `tv.py install --tag build-28`, `tv.py launch --fresh`, `tv.py jit`,
   `tv.py vfs mount`, `tv.py va --probe2` → `va-fresh.json`, `tv.py mem` →
   `mem-fresh.json`.

## Part A — C4

A1. B2 again, with `kill $!` (006's `kill %1` is ESRCH in a non-interactive
    dash, as you found):
    `tv.py exec --wait 90 -- /usr/bin/sh -c 'Xvfb :1 -screen 0 320x240x24 -nolisten tcp -extension MIT-SHM >/tmp/xvfb1.log 2>&1 & xp=$!; for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do [ -S /tmp/.X11-unix/X1 ] && break; sleep 1; done; ls -la /tmp/.X11-unix; DISPLAY=:1 xdpyinfo | head -12; kill $xp; wait; echo rc=$?'`
    → `X1`, xdpyinfo's `name of display: :1` … `vendor string: The X.Org
    Foundation`, `rc=0`, exit 0 within the 90 s → `a1.json/-stdout.txt`,
    `tv.py guest-file /tmp/xvfb1.log --out a1-xvfb1.log`.
A2. **C4 proper**: `tv.py exec --wait 180 -- /opt/rl/refs/c4.sh`. Expected
    output (timestamps vary):
    ```
    [hh:mm:ss] Xvfb up on :0 (pid N)
    name of display:    :0
    version number:    11.0
    vendor string:    The X.Org Foundation
    ...
    xwininfo: Window id: 0x... (the root window) (has no name)
      ...
         1 child:
         0x... "xeyes": ("xeyes" "XEyes")  400x300+100+100  +100+100
    [hh:mm:ss] snapshot /refs/out/c4.xwd
    C4 done
    ```
    exit 0. Save `c4.json`, `c4-stdout.txt`, `tv.py log --all --out c4-log.txt`
    (the whole run fits the ring now; note the `dropped` count),
    `tv.py guest-file /refs/out/xvfb.log --out c4-xvfb.log`,
    `tv.py guest-file /refs/out/c4.xwd --out c4.xwd` then `convert c4.xwd c4.png`,
    `tv.py shot --out c4-shot.png`, `tv.py mem` → `c4-mem.json` (once while
    Xvfb runs if convenient, once after).
A3. `tv.py ps` → `ps-a.json` (Xvfb, xeyes, xdotool, sh all `exited`,
    `mapped_bytes 0`), `tv.py va --probe2` → `va-a.json`, `tv.py mem` → `mem-a.json`.
A4. On a hang: `tv.py ps`, `tv.py log --all --out hang-log.txt`, `tv.py
    guest-file /refs/out/xvfb.log --out hang-xvfb.log`, `tv.py ps --killall`;
    which process is stuck and its last strace lines (for an epoll_wait that
    should have returned: the `epoll_ctl` lines for that fd and the ~30 lines
    before the hang).

**Part A PASS**: A2 prints xdpyinfo's report for `:0`, xwininfo lists the
`xeyes` window, `c4.png` shows the eyes, `C4 done`, exit 0, app alive.
**PARTIAL**: first deviating step + the last ~20 strace lines before it.
**FAIL**: app crash → `crash-N.txt` + the log tail.

## Part B — fault paths and df

Each `crashtest` step: `tv.py exec -- /opt/rl/bin/crashtest <mode>` → `b-<mode>.json/-stdout.txt`.

B1. `fork-untouched` → `fork with 4096 KB untouched`, `child: exit 0; parent
    sees m[0]=0 m[last]=0 (expect 0 0)`, exit **0**, no `FAULT` line in the
    log (build-27 killed such a child with SIGSEGV, `kind=3`).
B2. `ro-write` → exit **139** (`kind=3` in the log is expected: untouched page;
    the guest `si_code` will come from the VMA table when guest SIGSEGV
    delivery lands).
B3. `handler` → recorded, not judged (exit 139 expected until guest signal
    delivery from JIT code lands; `recovered: value=42 handled=1` would mean it
    already works).
B4. `tv.py exec -- /usr/bin/sh -c 'df /tmp / /proc /dev/shm /sys'` → `df.json`:
    `/tmp` and `/dev/shm` tmpfs ~1 GB mostly free, `/` ~48 GB with ~1 GB free,
    `/proc` and `/sys` all zeros (busybox may print `-` for them).
B5. `tv.py va --probe2` → `va-b.json` (within 0.3 GB of `va-fresh.json`),
    `tv.py mem` → `mem-b.json`, `tv.py log --all --out b-log.txt`.

**Part B PASS**: B1, B2, B4, B5 as stated.

## Files: `handoff/results/007-c4-epoll/`

`va-fresh.json`, `mem-fresh.json`; A: `a1.json/-stdout.txt`, `a1-xvfb1.log`,
`c4.json`, `c4-stdout.txt`, `c4-log.txt`, `c4-xvfb.log`, `c4.xwd`, `c4.png`,
`c4-shot.png`, `c4-mem.json`, `ps-a.json`, `va-a.json`, `mem-a.json`; B:
`b-<mode>.json/-stdout.txt` ×3, `df.json`, `va-b.json`, `mem-b.json`,
`b-log.txt`; `verdict.md`; `hang-*`/`crash-*` if any.

## Also

Issue 005 (laptop-only, the E1/E2 go/no-go experiment) stays the priority
whenever the laptop is free; its files go to `handoff/results/issue-005/`.
