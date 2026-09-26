# laptop/ — what LAPTOP runs

Everything here executes on the x86-64 Debian laptop. `laptop/config.env` is
LAPTOP's file (copy `config.env.example`; REPO never edits it). Paths must
not contain spaces.

## Order

```sh
sh laptop/setup/00-prereqs.sh      # apt packages, pyatv, user namespaces, config.env skeleton
#   edit laptop/config.env: TV_IP, TV_DEVICE_ID (atvremote scan), JIT_CMD, INSTALL_CMD, LAPTOP_IP
sh laptop/setup/10-rootfs.sh       # debootstrap + Xvfb/x11/busybox/strace/gcc/mingw/vulkan + WineHQ (sudo)
sh laptop/setup/20-wineprefix.sh   # 64-bit prefix at $WINEPREFIX_DIR, dxvk-macOS DLLs, registry tweaks
sh laptop/setup/30-game.sh         # link Rocket League → $ASSETS_DIR/game, seed TASystemSettings.ini
sh laptop/setup/40-assets.sh       # manifest (sha256, cached) + HTTP range server on $ASSETS_PORT
sh laptop/refs/run.sh c1 c2 c3 c4 c5 d1 d2 d3      # strace reference traces (Xvfb, like the TV)
sh laptop/refs/run.sh --display real e1 e2         # game references on the laptop's real display
```

## Per-checkpoint scripts

`laptop/refs/guest/<name>.sh` is the program under test. It runs unchanged in
two places: natively in the rootfs under `strace -f -tt` (via `run.sh`), and
on the Apple TV under the fake kernel. `run.sh` writes to `$REFS_DIR`:

- `<name>.trace.gz` — the strace reference the fake kernel is compared against
- `<name>.summary.txt` — syscall/errno digest (`laptop/refs/summarize.py`)
- `<name>.stdout`, `out/<name>/*.png|*.log` — program output and snapshots

Attach `<name>.summary.txt`, `<name>.stdout` and the PNGs to the result;
traces (`.trace.gz`) are committed when a request asks for them (they are
large; gzip keeps them manageable).

## Driving the TV

`tools/tv.py` (JSON output, see `--help`) does everything on the TV side:
`build`, `install`, `jit`, `launch`, `kill`, `status`, `shot`, `log`, `mem`,
`va`, `crash`, `input`, `run`, `cycle`, `result`. The debug server on the TV
listens on `TV_IP:7777`; the app fetches the guest filesystem from
`LAPTOP_IP:ASSETS_PORT`.

## Bubblewrap sandbox

`rootfs_run` (in `setup/lib.sh`) enters the rootfs rootless with the same
mount layout the TV uses: `/` rootfs (ro), `/prefix`, `/game` (ro),
`/home/user`, `/refs`, tmpfs `/tmp`. `--display real` adds the laptop's X
socket and `/dev/dri`. `rootfs_root_run` is only for apt (sudo chroot).
