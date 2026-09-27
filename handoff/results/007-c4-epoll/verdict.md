# 007-c4-epoll — build-28 on the Apple TV

**Part A (C4): PASS.**
- A1 (Xvfb :1 + xdpyinfo + `kill $!`): `rc=0`, exit 0 in 1.7 s.
- **A2 `c4.sh`: complete.** Xvfb comes up on :0 and xdpyinfo reports. xwininfo lists
  **`0x20000a "xeyes": ("xeyes" "XEyes")  400x300+100+100  +100+100`**, the snapshot is taken,
  `C4 done`, exit 0 in 4.8 s.
- **c4.png shows the eyes** on the black root window, pupils turned down-left toward xdotool's
  last pointer position (200, 600).
- The app stayed alive; no FAULT lines; log 8,374 lines, dropped 0.

**Part B (fault paths + df): PASS**, with a note on B5's two probes:
- B1 `fork-untouched`: `child: exit 0; parent sees m[0]=0 m[last]=0 (expect 0 0)`, exit 0, no
  FAULT line.
- B2 `ro-write`: exit 139 (`kind=3`).
- B4 `df`: all values as expected.
- B5: `va-b` in 64 MB steps is 7.19 vs 7.44 GB fresh (−0.25 GB, inside 0.3). In 256 MB steps it
  is 6.75 vs 7.25 (−0.5), but that drop already appeared at A3, after C4, and didn't move during
  Part B. It looks like address-space fragmentation from the X run, not a per-process leak.
- B3 `handler` (recorded): exit 139; the guest handler is not entered yet.

Build **build-28** (2cc2222, IPA sha256 c5909ce2…), app pid 1273, TV 07:07–07:11 CEST.
- **Prep:** `crashtest` rebuilt (customize.sh); native `fork-untouched` gives the expected line,
  exit 0. Manifest 70,401 entries.
- **Install and launch:** install 11 s; `launch --fresh` woke the TV and started on the first try;
  `jit` ready (4.28 s); `vfs mount` 387 ms.
- **Fresh baseline:** va-fresh 7.25 / 7.44 GB (256 / 64 MB steps); mem-fresh 191 MB.

## Part A
| step | result | files |
|---|---|---|
| A1 | **PASS**: `srwxrwxrwx … X1`, `name of display: :1`, `vendor string: The X.Org Foundation`, `X.Org version: 21.1.16`, …, `rc=0`, exit 0, 1.4 s (Xvfb's libraries are now in the TV's VFS block cache; in 006 they took ~15 s to load) | a1.json/-stdout.txt, a1-xvfb1.log (only `_XSERVTransmkdir: ERROR: euid != 0,directory /tmp/.X11-unix will not be created.`) |
| A2 | **PASS**: the output exactly as expected (c4-stdout.txt), with `xwininfo … 1 child: 0x20000a "xeyes" …` and `[05:08:45] snapshot /refs/out/c4.xwd`, `C4 done`, exit 0, 258 syscalls, 4.4 s | c4.json, c4-stdout.txt, c4-log.txt (count 8,374, **dropped 0**), c4-xvfb.log (0 bytes: Xvfb logged nothing), **c4.xwd** (3.7 MB) → **c4.png** (converted with the rootfs's ImageMagick), c4-shot.png, c4-mem.json (phys 246.5 MB, **peak 433.7 MB**) |
| A3 | 28 processes, all `exited`, `mapped_bytes 0`, `live_threads 0` (ps-a.json): Xvfb 0, xeyes 143 (the script's `kill`), xdpyinfo 141 (SIGPIPE from `head`), xkbcomp, xdotool, xwd, xwininfo, sh 0 | ps-a.json, va-a.json (6.75 / 7.19 GB), mem-a.json (246.5 MB) |

## Part B
| step | result |
|---|---|
| B1 `fork-untouched` | **PASS**: `fork with 4096 KB untouched` / `child: exit 0; parent sees m[0]=0 m[last]=0 (expect 0 0)`, exit 0. Log: `pid 29 fork -> child pid 30 (snapshot)`, `pid 30 … exited code=0 after 3 syscalls`, and **no FAULT line** |
| B2 `ro-write` | **PASS**: exit 139 (`term_signal 11`); `FAULT signal=10 code=1 kind=3 … addr=0x104304000 -> killed by signal 11` |
| B3 `handler` (recorded) | `writing to a read-only page 0x104304000 with a handler`, then exit 139 (`kind=3`): the handler is not entered yet |
| B4 `df` | **PASS**: `tmpfs 1048576 65536 983040 7% /tmp`, `rootfs 50331648 49283072 1048576 98% /`, `- 0 0 0 - /proc`, `tmpfs 1048576 65536 983040 7% /dev/shm`, `- 0 0 0 - /sys` |
| B5 | va-b **6.75 / 7.19 GB** (256 / 64 MB steps); mem-b 246.7 MB; b-log.txt (count 8,746, dropped 0). See the VA note above |

## Files
va-fresh.json, mem-fresh.json, launch.json, jit.json, vfs-mount.json;
A: a1.json/-stdout.txt, a1-xvfb1.log, c4.json, c4-stdout.txt, c4-log.txt, c4-xvfb.log, c4.xwd,
c4.png, c4-shot.png, c4-mem.json, ps-a.json, va-a.json, mem-a.json;
B: b-fork-untouched / b-ro-write / b-handler `.json`/`-stdout.txt`, df.json/-stdout.txt,
va-b.json, mem-b.json, b-log.txt.
