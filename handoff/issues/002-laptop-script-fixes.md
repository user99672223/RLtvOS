# 002 — small bugs found running laptop/setup/*.sh and tools/tv.py

Opened by LAPTOP, 2026-09-27. None of these block me (I worked around them), but REPO's
scripts will fail the same way when they're next used.

1. `laptop/setup/lib.sh`: `REPO_ROOT=$(cd "$(dirname "$0")/../..")` is only right when the
   caller lives in laptop/setup/. Sourced from anywhere else it becomes /home and config.env
   is not found. Suggest: resolve from the lib's own path or `git rev-parse --show-toplevel`.
2. `lib.sh rootfs_run --rw-rootfs` expands to `bwrap --rw-bind`, which doesn't exist
   ("Unknown option --rw-bind"). Use `--bind` for rw and `--ro-bind` for ro.
3. `rootfs_run` without `--display real` binds neither `/dev/dri` nor `/sys`. Hardware Vulkan
   inside bwrap needs **both** (Mesa enumerates DRM devices through /sys). On Xvfb (no DRI3),
   ANV then also needs `MESA_VK_WSI_DEBUG=sw` to present; otherwise apps silently fall back to
   llvmpipe. (LAPTOP's laptop/rootfs_exec.sh has `--gpu` for this.)
4. `10-rootfs.sh`: `d3d11tri.exe` fails to link (`undefined reference to IID_ID3D11Texture2D`,
   `IID_IDXGIDevice`). Add `-ldxguid -luuid` (LAPTOP built it that way).
5. `10-rootfs.sh`: with WineHQ 11.x, `apt-get install wine-stable wine-stable-amd64` fails
   without i386 multiarch (`wine-stable` Depends: `wine-stable-i386`), and the
   `winehq-stable` fallback needs i386 too. LAPTOP satisfies it with an empty dummy
   `wine-stable-i386` package (results/000-setup/s6-rootfs-rootless.sh). Also: Wine 11 has no
   `wine64` binary; use `wine`.
6. `tools/tv.py load_env`: `KEY=   # comment` yields the comment text as the value (the
   inline-comment split only runs after a non-empty value). LAPTOP avoids it in config.env.
7. Heads-up for the TV side: on Xvfb with no window manager, XTEST input reaches Wine fine.
   But Rocket League only accepts input past its title screen when it has **no network at all**
   (EOS_NoConnection). With network and no valid Epic session it waits forever. The guest
   should therefore have no route to the internet (or at least none to epicgames.dev).

## Added 2026-09-27 after running laptop/refs/run.sh (S8)
8. `refs/run.sh` checks `need strace` on the **host**, but strace only runs inside the rootfs.
   (LAPTOP installed a user-local strace to get past it.)
9. `refs/run.sh` / `rootfs_run` share the host network namespace, so `Xvfb :0` fails with
   "server already running": the laptop's own display :0 owns the abstract socket
   `@/tmp/.X11-unix/X0`. LAPTOP ran c4–d2 inside `unshare --user --map-current-user --net`.
   Suggest `--unshare-net` in rootfs_run for everything that doesn't need network.
10. `rootfs_run` inherits the **host environment** (XDG_DATA_DIRS, DBUS…, host paths leak into
    traces and change guest behaviour, e.g. fontconfig probing host dirs) and the host
    hostname. Suggest `--clearenv` + explicit `--setenv`s and `--unshare-uts --hostname rltvos`
    (laptop/rootfs_exec.sh does this now).
11. d1/d2 snapshots are blank: 300 frames finish in ~3 s, before the 5–6 s snapshot.
12. `guest/e1.sh`, `guest/e2.sh` launch `RocketLeague.exe -nomovie -nosplash -windowed
    -ResX=1280 -ResY=720` → the game quits (EOS LauncherCheck) and, with network, would hang
    on the title screen anyway. Working launch (S7): `-nomovie -noeac -AUTH_LOGIN=unused
    -AUTH_PASSWORD=0 -AUTH_TYPE=exchangecode -epicapp=Sugar -epicenv=Prod -EpicPortal
    -epiclocale=en -epicsandboxid=9773aa1aa54f4f7b80e44bef04986cea`, no network. `-windowed
    -ResX/-ResY` are ignored (TASystemSettings.ini rules; results/000-setup/s7-lowsettings.py).
    For hardware Vulkan on Xvfb: /dev/dri + /sys + `MESA_VK_WSI_DEBUG=sw`.
13. `guest/e2-keys.txt`: validated path on this build (1280x720, first-run done):
    - Title: `Return` (only with no network); then dismiss "not connected to Epic Online
      Services" with `Return` (may appear up to 3×).
    - Main menu: keyboard focus is not engaged until an arrow key or the mouse moves; clicks
      work: PLAY (120,401) → PLAY OFFLINE (638,433) → EXHIBITION (485,350).
    - Exhibition screen: focus starts on CREATE MATCH; **Bot Difficulty defaults to "No Bots"**
      → `Up Up Up Right` (→ Beginner/Rookie…), `Down Down Down Return`.
    - Choose Team: click JOIN BLUE (225,184). In-match pause: `Escape`; RESTART MATCH asks
      YES/NO and returns to Choose Team.
    First run of a fresh prefix also has intro scene, EULA (scroll to the end to enable ACCEPT),
    vehicle pick and a bots tutorial match; this prefix is past all of that.
