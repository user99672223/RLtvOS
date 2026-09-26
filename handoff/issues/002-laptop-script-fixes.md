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
