# refs/ — native strace references (LAPTOP, S8, 2026-09-27)

Every trace ran **natively** on the laptop inside the guest rootfs (Debian trixie
amd64, WineHQ 11.0 64-bit, DXVK-macOS v1.10.3-20230507-repack) under bubblewrap,
recorded with `strace -f -tt -y -s 160`, then gzipped. Next to each trace:
`<name>.stdout` (the program's output), `<name>.summary.txt` (REPO's
laptop/refs/summarize.py) and `out/<name>/` (logs, snapshots).

| name | what | lines | procs/threads | unique syscalls | ENOSYS | .gz | where |
|------|------|------:|------:|------:|--------|-----:|-------|
| c1 | static hello (write/exit_group) | 58 | 1 | 27 | none | 1.7 KB | here |
| c2 | dynamic glibc hello | 1,094 | 6 | 49 | none | 15 KB | here |
| c3 | busybox sh pipeline + background job + threads-test | 770 | 17 | 48 | none | 10 KB | here |
| c4 | Xvfb :0 + xdpyinfo + xeyes + xdotool | 6,889 | 19 | 67 | none | 102 KB | here |
| c5 | wine notepad under Xvfb (wineserver -f -p first) | 437,522 | 134 | 104 | none | 4.1 MB | here |
| d1 | vkcube (llvmpipe; no GPU in REPO's sandbox) | 117,471 | 55 | 83 | none | 0.9 MB | here |
| d2 | d3d11tri.exe via DXVK-macOS under wine (llvmpipe) | 848,568 | 731 | 109 | none | 8.1 MB | here |
| e1 | RocketLeague.exe: launch → title → offline login timeout → **60 s of main menu** | 24,511,288 | 180 | 102 | none | 197 MB | release |
| e2 | **first ~60 s of an exhibition vs bots** (Farmstead, 1v1, Rookie), from team join through kickoff and play | 8,519,081 | 80 | 53 | none | 68 MB | release |

`release` = too big for git (GitHub's 100 MB file limit). Fetch with:
```
gh release download refs-laptop-1 -R user99672223/RLtvOS -p 'e*.trace.gz' -D refs/
sha256sum -c refs/e12.sha256
```

## How they were made
- c1–d2: REPO's `laptop/refs/run.sh c1 … d2` unchanged, but run inside its own network
  namespace (`unshare --user --map-current-user --net laptop/refs/run.sh …`). With the
  host's network namespace, `Xvfb :0` collides with the laptop's own display `:0` through
  the shared abstract X socket. d1/d2 run on llvmpipe because REPO's `rootfs_run` binds
  no `/dev/dri` or `/sys`. The d1/d2 snapshots are blank: 300 frames finish in about 3 s,
  before the 5–6 s snapshot. The traces are complete.
- e1/e2: `handoff/results/000-setup/s8-game-traces.sh`, not REPO's e1.sh/e2.sh: those
  launch without `-noeac`/`-EpicPortal …` and with network, and then the game quits
  (LauncherCheck) or waits forever on its title screen. Launch = S7 known-good: no
  network, Intel ANV with `MESA_VK_WSI_DEBUG=sw` on Xvfb 1280x720 (no MIT-SHM),
  `wineserver -f -p` started first, then `wine RocketLeague.exe -nomovie -noeac
  -AUTH_LOGIN=unused -AUTH_PASSWORD=0 -AUTH_TYPE=exchangecode -epicapp=Sugar -epicenv=Prod
  -EpicPortal -epiclocale=en -epicsandboxid=…`, low settings, 30 fps cap.
  - e1: strace started the game; trace cut at menu+60 s (23:01:25). strace's default
    `-I3` for `-o FILE PROG` blocks SIGTERM, so it had to be killed and the tail trimmed.
  - e2: host strace attached to RocketLeague.exe and wineserver (`-p`) for 72 s starting at
    "JOIN BLUE". The -y annotations still show guest paths. Timestamps are host local time
    (CEST); e1's are sandbox UTC.
- Redaction: the laptop's username and hostname were replaced by `user` / `laptop` in all
  traces. They leaked through the inherited host environment (XDG_DATA_DIRS → fontconfig),
  REPO's stdout redirect, and uname. `laptop/rootfs_exec.sh` now uses `--clearenv` and
  its own UTS namespace (hostname `rltvos`), so new traces won't need this.
- Not run: d3 (evtest with a uinput virtual pad) needs sudo; the user's S8 list doesn't
  include it.
