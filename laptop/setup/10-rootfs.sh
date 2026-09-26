#!/bin/sh
# 10-rootfs.sh — build the Debian amd64 rootfs the guest runs from:
#   minbase + Xvfb/x11 tools/busybox/strace/gcc/mingw/vulkan
#   + WineHQ wine-$WINE_BRANCH (64-bit only; the amd64 package carries the
#   i386 PE side for new-WoW64) + checkpoint binaries in /opt/rl/bin.
#
# Two modes (ROOTFS_MODE=auto|rootless|sudo, default auto):
#   rootless  mmdebstrap --mode=unshare → tar → extracted as $USER (no sudo
#             anywhere; device nodes/setuid dropped, which the guest never
#             needs). Incremental package installs then go through
#             rootfs_fakeroot_run (bwrap user namespace, uid 0 = $USER).
#   sudo      debootstrap + sudo chroot apt, chowned to $USER at the end.
# Idempotent: with an existing rootfs only the package set and /opt/rl are
# refreshed. Full rebuild: ROOTFS_REBUILD=1.
set -eu
. "$(dirname "$0")/lib.sh"

PKGS="busybox-static xvfb x11-utils x11-apps x11-xserver-utils xdotool evtest strace gdb-minimal \
gcc libc6-dev make mingw-w64 \
vulkan-tools libvulkan1 mesa-vulkan-drivers libgl1 libegl1 \
libsdl2-2.0-0 libfreetype6 libgnutls30 libxcomposite1 libxcursor1 libxi6 libxrandr2 libxrender1 \
libxfixes3 libxext6 libxinerama1 libfontconfig1 fonts-dejavu-core \
libgstreamer1.0-0 libgstreamer-plugins-base1.0-0 \
ca-certificates gnupg curl wget procps python3-minimal file less nano"
WINE_PKGS="wine-$WINE_BRANCH wine-$WINE_BRANCH-amd64"
DL="$ASSETS_DIR/downloads"
mkdir -p "$DL"

mode=${ROOTFS_MODE:-auto}
if [ "$mode" = auto ]; then
  if sudo -n true 2>/dev/null; then mode=sudo; elif have mmdebstrap; then mode=rootless
  else die "need either non-interactive sudo (debootstrap path) or mmdebstrap (rootless path): apt install mmdebstrap"; fi
fi
log "rootfs mode: $mode  →  $ROOTFS_DIR"

# WineHQ key (used by both modes).
[ -s "$DL/winehq-archive.key" ] || curl -fsSL https://dl.winehq.org/wine-builds/winehq.key -o "$DL/winehq-archive.key"
SRC_MAIN="deb $DEBIAN_MIRROR $DEBIAN_SUITE main contrib non-free non-free-firmware"
SRC_UPD="deb $DEBIAN_MIRROR $DEBIAN_SUITE-updates main contrib non-free non-free-firmware"
SRC_SEC="deb http://security.debian.org/debian-security $DEBIAN_SUITE-security main contrib non-free non-free-firmware"
SRC_WINE="deb [signed-by=/etc/apt/keyrings/winehq-archive.key] https://dl.winehq.org/wine-builds/debian/ $DEBIAN_SUITE main"

if [ "${ROOTFS_REBUILD:-0}" = 1 ] && [ -d "$ROOTFS_DIR" ]; then
  log "ROOTFS_REBUILD=1: removing $ROOTFS_DIR"
  rm -rf "$ROOTFS_DIR" 2>/dev/null || sudo rm -rf "$ROOTFS_DIR"
fi

# ------------------------------------------------------------------ rootless
if [ "$mode" = rootless ]; then
  need mmdebstrap; need bwrap
  if [ ! -d "$ROOTFS_DIR/usr" ]; then
    TAR="$DL/rootfs-$DEBIAN_SUITE.tar"
    log "mmdebstrap --mode=unshare → $TAR (this takes a while: wine is large)"
    pkgs_csv=$(echo "$PKGS $WINE_PKGS" | tr -s ' ' ',')
    mmdebstrap --mode=unshare --variant=minbase --arch=amd64 \
      --include="$pkgs_csv" \
      --aptopt='Acquire::Retries "3"' \
      --dpkgopt='path-exclude=/usr/share/doc/*' --dpkgopt='path-exclude=/usr/share/man/*' \
      --setup-hook="upload $DL/winehq-archive.key /etc/apt/keyrings/winehq-archive.key" \
      --customize-hook="copy-in $REPO_ROOT/laptop/refs/guest/src /opt/rl" \
      --customize-hook="upload $REPO_ROOT/laptop/setup/rootfs-customize.sh /opt/rl/customize.sh" \
      --customize-hook='chroot "$1" sh /opt/rl/customize.sh' \
      "$DEBIAN_SUITE" "$TAR" "$SRC_MAIN" "$SRC_UPD" "$SRC_SEC" "$SRC_WINE"
    log "extracting as $(id -un) (ownership dropped on purpose)"
    mkdir -p "$ROOTFS_DIR"
    tar -xf "$TAR" -C "$ROOTFS_DIR" --no-same-owner --no-same-permissions --exclude='./dev/*' 2>&1 | grep -v "Cannot mknod" || true
    chmod -R u+rwX "$ROOTFS_DIR"
    rm -f "$TAR"
  else
    log "rootfs exists; refreshing packages and /opt/rl via user-namespace apt"
    printf '%s\n%s\n%s\n' "$SRC_MAIN" "$SRC_UPD" "$SRC_SEC" > "$ROOTFS_DIR/etc/apt/sources.list"
    mkdir -p "$ROOTFS_DIR/etc/apt/keyrings" "$ROOTFS_DIR/etc/apt/sources.list.d"
    cp "$DL/winehq-archive.key" "$ROOTFS_DIR/etc/apt/keyrings/winehq-archive.key"
    echo "$SRC_WINE" > "$ROOTFS_DIR/etc/apt/sources.list.d/winehq.list"
    cp -L /etc/resolv.conf "$ROOTFS_DIR/etc/resolv.conf" 2>/dev/null || true
    # shellcheck disable=SC2086
    rootfs_fakeroot_run sh -c "apt-get update -qq && apt-get install -y --no-install-recommends $PKGS $WINE_PKGS"
    rm -rf "$ROOTFS_DIR/opt/rl/src"; mkdir -p "$ROOTFS_DIR/opt/rl"
    cp -r "$REPO_ROOT/laptop/refs/guest/src" "$ROOTFS_DIR/opt/rl/src"
    cp "$REPO_ROOT/laptop/setup/rootfs-customize.sh" "$ROOTFS_DIR/opt/rl/customize.sh"
    rootfs_fakeroot_run sh /opt/rl/customize.sh
  fi
fi

# ------------------------------------------------------------------ sudo
if [ "$mode" = sudo ]; then
  need sudo; need debootstrap
  if [ ! -d "$ROOTFS_DIR/usr" ]; then
    log "debootstrap $DEBIAN_SUITE amd64 → $ROOTFS_DIR"
    sudo mkdir -p "$ROOTFS_DIR"
    sudo debootstrap --arch=amd64 --variant=minbase --include=ca-certificates,gnupg,curl \
      "$DEBIAN_SUITE" "$ROOTFS_DIR" "$DEBIAN_MIRROR"
  fi
  printf '%s\n%s\n%s\n' "$SRC_MAIN" "$SRC_UPD" "$SRC_SEC" | sudo tee "$ROOTFS_DIR/etc/apt/sources.list" >/dev/null
  sudo mkdir -p "$ROOTFS_DIR/etc/apt/keyrings" "$ROOTFS_DIR/etc/apt/sources.list.d"
  sudo cp "$DL/winehq-archive.key" "$ROOTFS_DIR/etc/apt/keyrings/winehq-archive.key"
  echo "$SRC_WINE" | sudo tee "$ROOTFS_DIR/etc/apt/sources.list.d/winehq.list" >/dev/null
  # shellcheck disable=SC2086
  rootfs_root_run sh -c "apt-get update -qq && apt-get install -y --no-install-recommends $PKGS $WINE_PKGS"
  sudo rm -rf "$ROOTFS_DIR/opt/rl/src"; sudo mkdir -p "$ROOTFS_DIR/opt/rl"
  sudo cp -r "$REPO_ROOT/laptop/refs/guest/src" "$ROOTFS_DIR/opt/rl/src"
  sudo cp "$REPO_ROOT/laptop/setup/rootfs-customize.sh" "$ROOTFS_DIR/opt/rl/customize.sh"
  rootfs_root_run sh /opt/rl/customize.sh
  log "chown → $(id -un) so bwrap/assets server run unprivileged"
  sudo chown -R "$(id -u):$(id -g)" "$ROOTFS_DIR"
fi

log "rootfs summary: $(du -sh "$ROOTFS_DIR" | cut -f1)"
rootfs_run -- sh -c 'echo "wine: $(command -v wine64 || command -v wine) $(wine --version 2>/dev/null || wine64 --version 2>/dev/null)"; Xvfb -version 2>&1 | head -1; busybox 2>&1 | head -1; /opt/rl/bin/hello-static; /opt/rl/bin/hello-dyn | head -2'
