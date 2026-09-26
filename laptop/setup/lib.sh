#!/bin/sh
# lib.sh — shared helpers for laptop/setup/*.sh and laptop/refs/run.sh.
# POSIX sh. Sourced, not executed. LAPTOP runs these on the Debian laptop.
# Paths in config.env must not contain spaces.

# Repo root: from git if possible (works when sourced from anywhere), else
# from the caller's location (laptop/setup or laptop/refs).
REPO_ROOT=$(git rev-parse --show-toplevel 2>/dev/null || true)
if [ -z "$REPO_ROOT" ] || [ ! -d "$REPO_ROOT/laptop" ]; then
  REPO_ROOT=$(cd "$(dirname "$0")/../.." 2>/dev/null && pwd)
fi
[ -d "$REPO_ROOT/laptop" ] || REPO_ROOT=$(pwd)
CONFIG_ENV=${CONFIG_ENV:-$REPO_ROOT/laptop/config.env}

die() { echo "ERROR: $*" >&2; exit 1; }
log() { echo "[$(date +%H:%M:%S)] $*" >&2; }
need() { command -v "$1" >/dev/null 2>&1 || die "missing tool: $1 (run laptop/setup/00-prereqs.sh)"; }
have() { command -v "$1" >/dev/null 2>&1; }

# Load laptop/config.env (KEY=VALUE, shell syntax) if present.
if [ -f "$CONFIG_ENV" ]; then
  set -a
  # shellcheck disable=SC1090
  . "$CONFIG_ENV"
  set +a
else
  log "note: $CONFIG_ENV not found; using defaults (copy laptop/config.env.example)"
fi

ASSETS_DIR=${ASSETS_DIR:-$HOME/rltvos/assets}
ROOTFS_DIR=${ROOTFS_DIR:-$ASSETS_DIR/rootfs}
WINEPREFIX_DIR=${WINEPREFIX_DIR:-$ASSETS_DIR/prefix}
HOME_DIR=${HOME_DIR:-$ASSETS_DIR/home}
REFS_DIR=${REFS_DIR:-$HOME/rltvos/refs}
OUT_DIR=${OUT_DIR:-$HOME/rltvos/out}
DEBIAN_SUITE=${DEBIAN_SUITE:-trixie}
DEBIAN_MIRROR=${DEBIAN_MIRROR:-http://deb.debian.org/debian}
WINE_BRANCH=${WINE_BRANCH:-stable}
ASSETS_PORT=${ASSETS_PORT:-8090}
GUEST_PATH=/opt/wine-$WINE_BRANCH/bin:/opt/rl/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

mkdir -p "$ASSETS_DIR" "$REFS_DIR" "$OUT_DIR" "$HOME_DIR" 2>/dev/null || true

# Prints the Rocket League install dir (GAME_DIR, else Heroic/Legendary metadata).
detect_game_dir() {
  if [ -n "$GAME_DIR" ] && [ -d "$GAME_DIR" ]; then echo "$GAME_DIR"; return 0; fi
  if [ -L "$ASSETS_DIR/game" ] && [ -d "$ASSETS_DIR/game" ]; then echo "$ASSETS_DIR/game"; return 0; fi
  for f in "$HOME/.config/heroic/legendaryConfig/legendary/installed.json" "$HOME/.config/legendary/installed.json"; do
    [ -f "$f" ] || continue
    d=$(python3 - "$f" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
for k, v in d.items():
    if k == "Sugar" or "rocket" in (v.get("title") or "").lower():
        print(v.get("install_path", "")); break
EOF
)
    if [ -n "$d" ] && [ -d "$d" ]; then echo "$d"; return 0; fi
  done
  if have legendary; then
    d=$(legendary list-installed --json 2>/dev/null | python3 -c 'import json,sys
for g in json.load(sys.stdin):
    if g.get("app_name")=="Sugar" or "rocket" in g.get("title","").lower(): print(g["install_path"]); break' 2>/dev/null)
    if [ -n "$d" ] && [ -d "$d" ]; then echo "$d"; return 0; fi
  fi
  return 1
}

# rootfs_run [--rw-rootfs] [--display real|none] [--gpu] [--nonet] [--env K=V ...] [--] cmd args...
# Runs a command inside the rootfs with bubblewrap (rootless, current uid):
#   /            rootfs (read-only unless --rw-rootfs)
#   /prefix      Wine prefix (rw)          /game        Rocket League (ro, if found)
#   /home/user   writable home             /refs        $REFS_DIR (rw)
#   /refs/guest  laptop/refs/guest (ro)    /tmp,/run    tmpfs;  /dev,/proc fresh
# --gpu binds /dev/dri and /sys (Mesa enumerates DRM devices through /sys) and
#   sets MESA_VK_WSI_DEBUG=sw so ANV can present on Xvfb (no DRI3). Default for
#   d1/d2/e1/e2 in refs/run.sh.
# --nonet gives the sandbox its own network namespace (loopback only): Rocket
#   League only accepts input past the title screen with no network at all.
# --display real binds the laptop's X socket (+ /dev/dri) instead of Xvfb.
rootfs_run() {
  need bwrap
  rw=--ro-bind; disp=none; envs=""; extra=""
  while [ $# -gt 0 ]; do
    case "$1" in
      --rw-rootfs) rw=--bind; shift ;;
      --display) disp=$2; shift 2 ;;
      --gpu) extra="$extra --dev-bind /dev/dri /dev/dri --ro-bind /sys /sys --setenv MESA_VK_WSI_DEBUG sw"; shift ;;
      --nonet) extra="$extra --unshare-net"; shift ;;
      --env) envs="$envs --setenv ${2%%=*} ${2#*=}"; shift 2 ;;
      --) shift; break ;;
      *) break ;;
    esac
  done
  [ -d "$ROOTFS_DIR/usr" ] || die "rootfs not built at $ROOTFS_DIR (run laptop/setup/10-rootfs.sh)"
  mkdir -p "$WINEPREFIX_DIR" "$HOME_DIR" "$REFS_DIR"
  gameopts=""
  game=$(detect_game_dir 2>/dev/null || true)
  [ -n "$game" ] && gameopts="--ro-bind $game /game"
  dispopts=""
  if [ "$disp" = real ]; then
    [ -n "$DISPLAY" ] || die "--display real but \$DISPLAY is empty"
    dispopts="--ro-bind /tmp/.X11-unix /tmp/.X11-unix --setenv DISPLAY $DISPLAY"
    [ -d /dev/dri ] && dispopts="$dispopts --dev-bind /dev/dri /dev/dri --ro-bind /sys /sys"
    if [ -n "$XAUTHORITY" ] && [ -f "$XAUTHORITY" ]; then
      dispopts="$dispopts --ro-bind $XAUTHORITY /home/user/.Xauthority --setenv XAUTHORITY /home/user/.Xauthority"
    fi
  fi
  # shellcheck disable=SC2086
  bwrap $rw "$ROOTFS_DIR" / \
    --dev /dev --proc /proc --tmpfs /tmp --tmpfs /run --tmpfs /var/tmp \
    --bind "$WINEPREFIX_DIR" /prefix --bind "$HOME_DIR" /home/user --bind "$REFS_DIR" /refs \
    --ro-bind "$REPO_ROOT/laptop/refs/guest" /refs/guest \
    $gameopts $dispopts $extra ${EXTRA_BWRAP:-} \
    --setenv HOME /home/user --setenv USER user --setenv LOGNAME user \
    --setenv PATH "$GUEST_PATH" --setenv WINEPREFIX /prefix --setenv WINEARCH win64 \
    --setenv LANG C.UTF-8 --setenv TERM xterm \
    --unshare-pid --die-with-parent --chdir /home/user \
    $envs -- "$@"
}

# rootfs_fakeroot_run cmd args... — rootless "root": bwrap user namespace
# with uid/gid 0 mapped to the current user, rootfs read-write, network
# shared. Enough for apt-get/dpkg (files end up owned by $USER; chown to
# other users inside fails silently, which the guest never needs).
rootfs_fakeroot_run() {
  need bwrap
  [ -d "$ROOTFS_DIR/usr" ] || die "rootfs not built at $ROOTFS_DIR"
  bwrap --unshare-user --uid 0 --gid 0 --bind "$ROOTFS_DIR" / \
    --dev /dev --proc /proc --tmpfs /tmp --tmpfs /run \
    --ro-bind /etc/resolv.conf /etc/resolv.conf \
    --setenv HOME /root --setenv PATH "$GUEST_PATH" --setenv LANG C.UTF-8 \
    --setenv DEBIAN_FRONTEND noninteractive --setenv APT_CONFIG /dev/null \
    --unshare-pid --die-with-parent --chdir / -- "$@"
}

# rootfs_root_run cmd args... — same tree, but as root via sudo chroot (for
# apt inside the rootfs). Mounts /proc, /dev, /sys temporarily.
rootfs_root_run() {
  need sudo
  sudo mount -t proc proc "$ROOTFS_DIR/proc" 2>/dev/null || true
  sudo mount --bind /dev "$ROOTFS_DIR/dev" 2>/dev/null || true
  sudo mount --bind /dev/pts "$ROOTFS_DIR/dev/pts" 2>/dev/null || true
  sudo mount -t sysfs sys "$ROOTFS_DIR/sys" 2>/dev/null || true
  [ -f /etc/resolv.conf ] && sudo cp -L /etc/resolv.conf "$ROOTFS_DIR/etc/resolv.conf"
  rc=0
  sudo env -i HOME=/root PATH="$GUEST_PATH" LANG=C.UTF-8 DEBIAN_FRONTEND=noninteractive \
    chroot "$ROOTFS_DIR" "$@" || rc=$?
  sudo umount "$ROOTFS_DIR/sys" 2>/dev/null || true
  sudo umount "$ROOTFS_DIR/dev/pts" 2>/dev/null || true
  sudo umount "$ROOTFS_DIR/dev" 2>/dev/null || true
  sudo umount "$ROOTFS_DIR/proc" 2>/dev/null || true
  return $rc
}
