#!/bin/sh
# rootfs_exec.sh — LAPTOP: run a command inside the guest rootfs with bubblewrap
# (rootless). Same layout as laptop/setup/lib.sh's rootfs_run, plus:
#   --rw        rootfs writable (lib.sh's --rw-rootfs emits the invalid "--rw-bind")
#   --gpu       bind /dev/dri + /sys (hardware Vulkan with Xvfb, no host display)
#   --bind SRC DST / --ro-bind SRC DST   extra binds
#   --env K=V   extra environment
#   --nonet     no network (own network namespace; loopback only)
# Layout: /=rootfs  /prefix=WINEPREFIX_DIR(rw)  /home/user=HOME_DIR(rw)
#         /game=GAME_DIR(ro)  /refs=REFS_DIR(rw)  /refs/guest=laptop/refs/guest(ro)
#         /tmp,/run,/var/tmp tmpfs; fresh /dev,/proc; uid 1000 "user".
# The host environment is NOT inherited (--clearenv): only the variables set
# here and via --env reach the guest, as on the TV.
# usage: laptop/rootfs_exec.sh [opts] -- cmd args...
set -eu
REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
set -a; . "$REPO_ROOT/laptop/config.env"; set +a
rw=--ro-bind; extra=""
while [ $# -gt 0 ]; do
  case "$1" in
    --rw) rw=--bind; shift ;;
    --gpu) extra="$extra --dev-bind /dev/dri /dev/dri --ro-bind /sys /sys"; shift ;;
    --bind) extra="$extra --bind $2 $3"; shift 3 ;;
    --ro-bind) extra="$extra --ro-bind $2 $3"; shift 3 ;;
    --env) extra="$extra --setenv ${2%%=*} ${2#*=}"; shift 2 ;;
    --nonet) extra="$extra --unshare-net"; shift ;;
    --) shift; break ;;
    *) break ;;
  esac
done
mkdir -p "$WINEPREFIX_DIR" "$HOME_DIR" "$REFS_DIR"
# shellcheck disable=SC2086
exec bwrap $rw "$ROOTFS_DIR" / \
  --dev /dev --proc /proc --tmpfs /tmp --tmpfs /run --tmpfs /var/tmp \
  --bind "$WINEPREFIX_DIR" /prefix --bind "$HOME_DIR" /home/user --bind "$REFS_DIR" /refs \
  --ro-bind "$REPO_ROOT/laptop/refs/guest" /refs/guest \
  --ro-bind "$GAME_DIR" /game \
  --clearenv \
  --setenv HOME /home/user --setenv USER user --setenv LOGNAME user \
  --setenv PATH "/opt/wine-$WINE_BRANCH/bin:/opt/rl/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
  --setenv WINEPREFIX /prefix --setenv WINEARCH win64 --setenv LANG C.UTF-8 --setenv TERM xterm \
  --unshare-pid --unshare-uts --hostname rltvos --die-with-parent --chdir /home/user \
  $extra -- "$@"
