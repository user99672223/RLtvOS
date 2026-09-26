#!/bin/sh
# s6-rootfs-rootless.sh — exactly how LAPTOP built the guest rootfs (S6), without
# sudo. REPO: fold this into laptop/setup/10-rootfs.sh if you like; it replaces
# `sudo debootstrap` + `sudo chroot apt-get` + `sudo chown -R $USER`:
#   1. mmdebstrap --mode=unshare (user namespaces + /etc/subuid) → tarball
#   2. extract the tarball as the normal user → whole tree owned by $USER
#      (same end state as 10-rootfs.sh's chown; bwrap + assets server need no root)
# Package list = 10-rootfs.sh's debootstrap --include + PKGS + wine, plus
# imagemagick (laptop-side Xvfb captures). No i386 multiarch.
set -eu
REPO_ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
set -a; . "$REPO_ROOT/laptop/config.env"; set +a
MMDEBSTRAP=${MMDEBSTRAP:-mmdebstrap}
SUITE=${DEBIAN_SUITE:-trixie}
MIRROR=${DEBIAN_MIRROR:-http://deb.debian.org/debian}
BRANCH=${WINE_BRANCH:-stable}
BUILD=$HOME/rltvos/build
TAR=$BUILD/rootfs-$SUITE.tar
mkdir -p "$BUILD"

# The unshared root cannot read $HOME, so everything mmdebstrap's apt/dpkg must
# read lives in a world-readable dir:
#  - the dearmored WineHQ key (signed-by; a customize hook copies it into the
#    rootfs for later apt use)
#  - a dummy wine-$BRANCH-i386 package: WineHQ 11.x's wine-$BRANCH (which holds
#    bin/wine, wineserver, share/wine) hard-depends on wine-$BRANCH-i386 (690 MB
#    of 32-bit libs). The rootfs is 64-bit only, so an empty amd64 package with
#    that name and version satisfies it and keeps dpkg consistent.
KEYDIR=/var/tmp/rltvos-build
mkdir -p "$KEYDIR"; chmod 755 "$KEYDIR"
curl -fsSL https://dl.winehq.org/wine-builds/winehq.key | gpg --dearmor > "$KEYDIR/winehq.gpg"
WINEVER=$(curl -fsSL "https://dl.winehq.org/wine-builds/debian/dists/$SUITE/main/binary-amd64/Packages.gz" | zcat |
  awk -v p="wine-$BRANCH" '$1=="Package:"{n=$2} $1=="Version:" && n==p {print $2}' | sort -V | tail -1)
DUMMY=$KEYDIR/wine-$BRANCH-i386-dummy_${WINEVER}_amd64.deb
if [ ! -f "$DUMMY" ]; then
  D=$(mktemp -d); mkdir -p "$D/DEBIAN"
  printf 'Package: wine-%s-i386\nVersion: %s\nArchitecture: amd64\nMaintainer: RLtvOS laptop <noreply@example.invalid>\nSection: otherosfs\nPriority: optional\nDescription: dummy: 64-bit-only RLtvOS rootfs, no i386 Wine\n' "$BRANCH" "$WINEVER" > "$D/DEBIAN/control"
  dpkg-deb --root-owner-group -b "$D" "$DUMMY" >/dev/null; rm -rf "$D"
fi
chmod 644 "$KEYDIR"/*.gpg "$KEYDIR"/*.deb
MMHOOKS=${MMHOOKS:-/usr/share/mmdebstrap/hooks}

PKGS="ca-certificates,gnupg,curl,wget,procps,locales-all"
PKGS="$PKGS,busybox-static,xvfb,x11-utils,x11-apps,x11-xserver-utils,xdotool,evtest,strace,gdb-minimal"
PKGS="$PKGS,gcc,libc6-dev,make,mingw-w64"
PKGS="$PKGS,vulkan-tools,libvulkan1,mesa-vulkan-drivers,libgl1,libegl1"
PKGS="$PKGS,libsdl2-2.0-0,libfreetype6,libgnutls30t64,libxcomposite1,libxcursor1,libxi6,libxrandr2,libxrender1"
PKGS="$PKGS,libxfixes3,libxext6,libxinerama1,libfontconfig1,fonts-dejavu-core"
PKGS="$PKGS,libgstreamer1.0-0,libgstreamer-plugins-base1.0-0"
PKGS="$PKGS,python3-minimal,file,less,nano,imagemagick"
PKGS="$PKGS,wine-$BRANCH,wine-$BRANCH-amd64"

"$MMDEBSTRAP" --mode=unshare --variant=minbase --format=tar \
  --hook-dir="$MMHOOKS/file-mirror-automount" \
  --include="$PKGS" --include="$DUMMY" \
  --customize-hook="mkdir -p \"\$1/etc/apt/keyrings\" && cp $KEYDIR/winehq.gpg \"\$1/etc/apt/keyrings/winehq.gpg\" && sed -i 's#signed-by=$KEYDIR/winehq.gpg#signed-by=/etc/apt/keyrings/winehq.gpg#' \"\$1/etc/apt/sources.list\" || true" \
  "$SUITE" "$TAR" \
  "deb $MIRROR $SUITE main contrib non-free non-free-firmware" \
  "deb $MIRROR $SUITE-updates main contrib non-free non-free-firmware" \
  "deb http://security.debian.org/debian-security $SUITE-security main contrib non-free non-free-firmware" \
  "deb [signed-by=$KEYDIR/winehq.gpg] https://dl.winehq.org/wine-builds/debian/ $SUITE main"

# Extract as the normal user: every file ends up owned by $USER (device nodes
# are skipped — the guest's /dev is synthetic and bwrap provides its own).
rm -rf "$ROOTFS_DIR.new"; mkdir -p "$ROOTFS_DIR.new"
tar -C "$ROOTFS_DIR.new" --exclude='./dev/*' -xf "$TAR"
chmod -R u+rwX "$ROOTFS_DIR.new"
if [ -d "$ROOTFS_DIR" ]; then mv "$ROOTFS_DIR" "$ROOTFS_DIR.old.$(date +%s)"; fi
mv "$ROOTFS_DIR.new" "$ROOTFS_DIR"

# Same /etc tweaks as 10-rootfs.sh (user uid 1000 = the laptop user under bwrap).
R=$ROOTFS_DIR
grep -q '^user:' "$R/etc/passwd" || echo 'user:x:1000:1000:user:/home/user:/bin/sh' >> "$R/etc/passwd"
grep -q '^user:' "$R/etc/group" || echo 'user:x:1000:' >> "$R/etc/group"
mkdir -p "$R/home/user" "$R/prefix" "$R/game" "$R/refs" "$R/tmp/.X11-unix" "$R/opt/rl/bin" "$R/opt/rl/src"
chmod 1777 "$R/tmp" "$R/tmp/.X11-unix"
echo rltvos > "$R/etc/hostname"
printf '127.0.0.1 localhost\n127.0.1.1 rltvos\n' > "$R/etc/hosts"
[ -s "$R/etc/machine-id" ] || echo 9d3b1c1a2e4f4c0aa1f5b2c3d4e5f607 > "$R/etc/machine-id"
du -sh "$R"
