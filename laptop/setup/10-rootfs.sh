#!/bin/sh
# 10-rootfs.sh — build the Debian amd64 rootfs the guest runs from.
#   debootstrap (minbase) + Xvfb/x11 tools/busybox/strace/gcc/mingw/vulkan
#   + WineHQ wine-$WINE_BRANCH (64-bit only; the amd64 package carries the
#   i386 PE side for new-WoW64) + checkpoint binaries in /opt/rl/bin.
# Idempotent: re-running only (re)installs packages and rebuilds /opt/rl.
# Needs sudo (debootstrap/apt); the tree is chowned to $USER at the end so
# bwrap and the assets server work without root.
set -eu
. "$(dirname "$0")/lib.sh"
need sudo; need debootstrap

if [ ! -d "$ROOTFS_DIR/usr" ]; then
  log "debootstrap $DEBIAN_SUITE amd64 → $ROOTFS_DIR"
  sudo mkdir -p "$ROOTFS_DIR"
  sudo debootstrap --arch=amd64 --variant=minbase \
    --include=ca-certificates,gnupg,curl,wget,procps,locales-all \
    "$DEBIAN_SUITE" "$ROOTFS_DIR" "$DEBIAN_MIRROR"
else
  log "rootfs exists at $ROOTFS_DIR; updating packages"
fi

# apt sources: main contrib non-free-firmware + WineHQ.
sudo tee "$ROOTFS_DIR/etc/apt/sources.list" >/dev/null <<EOF
deb $DEBIAN_MIRROR $DEBIAN_SUITE main contrib non-free non-free-firmware
deb $DEBIAN_MIRROR $DEBIAN_SUITE-updates main contrib non-free non-free-firmware
deb http://security.debian.org/debian-security $DEBIAN_SUITE-security main contrib non-free non-free-firmware
EOF
sudo mkdir -p "$ROOTFS_DIR/etc/apt/keyrings"
if [ ! -f "$ROOTFS_DIR/etc/apt/keyrings/winehq-archive.key" ]; then
  curl -fsSL https://dl.winehq.org/wine-builds/winehq.key | sudo tee "$ROOTFS_DIR/etc/apt/keyrings/winehq-archive.key" >/dev/null
fi
curl -fsSL "https://dl.winehq.org/wine-builds/debian/dists/$DEBIAN_SUITE/winehq-$DEBIAN_SUITE.sources" | sudo tee "$ROOTFS_DIR/etc/apt/sources.list.d/winehq-$DEBIAN_SUITE.sources" >/dev/null

# Packages. No i386 multiarch on purpose: 64-bit-only Wine.
PKGS="busybox-static xvfb x11-utils x11-apps x11-xserver-utils xdotool evtest strace gdb-minimal \
 gcc libc6-dev make mingw-w64 \
 vulkan-tools libvulkan1 mesa-vulkan-drivers libgl1 libegl1 \
 libsdl2-2.0-0 libfreetype6 libgnutls30 libxcomposite1 libxcursor1 libxi6 libxrandr2 libxrender1 libxfixes3 libxext6 libxinerama1 libfontconfig1 fonts-dejavu-core \
 libgstreamer1.0-0 libgstreamer-plugins-base1.0-0 \
 python3-minimal file less nano"
rootfs_root_run sh -c "apt-get update -qq && apt-get install -y --no-install-recommends $PKGS"
rootfs_root_run sh -c "apt-get install -y --no-install-recommends wine-$WINE_BRANCH wine-$WINE_BRANCH-amd64 || apt-get install -y --install-recommends winehq-$WINE_BRANCH"
rootfs_root_run sh -c "apt-get clean && rm -rf /var/lib/apt/lists/*"

# Checkpoint binaries.
log "building /opt/rl/bin"
sudo mkdir -p "$ROOTFS_DIR/opt/rl/bin" "$ROOTFS_DIR/opt/rl/src"
sudo cp "$REPO_ROOT"/laptop/refs/guest/src/*.c "$ROOTFS_DIR/opt/rl/src/"
rootfs_root_run sh -c '
set -e
cd /opt/rl/src
gcc -static -nostdlib -nostartfiles -O2 -o /opt/rl/bin/hello-static hello_static.c
gcc -O2 -o /opt/rl/bin/hello-dyn hello_dyn.c
gcc -O2 -pthread -o /opt/rl/bin/threads-test threads_test.c
x86_64-w64-mingw32-gcc -O2 -municode -o /opt/rl/bin/d3d11tri.exe d3d11tri.c -ld3d11 -ldxgi -ld3dcompiler -luser32 -lgdi32 || echo "d3d11tri build failed (mingw d3dcompiler?)"
ls -la /opt/rl/bin
file /opt/rl/bin/* || true
'

# Minimal /etc for the guest: passwd/group with uid 0 and a user, hosts, machine-id.
rootfs_root_run sh -c '
set -e
grep -q "^user:" /etc/passwd || echo "user:x:1000:1000:user:/home/user:/bin/sh" >> /etc/passwd
grep -q "^user:" /etc/group || echo "user:x:1000:" >> /etc/group
mkdir -p /home/user /prefix /game /refs /tmp/.X11-unix
chmod 1777 /tmp /tmp/.X11-unix
echo rltvos > /etc/hostname
printf "127.0.0.1 localhost\n127.0.1.1 rltvos\n" > /etc/hosts
[ -s /etc/machine-id ] || printf "%s\n" "9d3b1c1a2e4f4c0aa1f5b2c3d4e5f607" > /etc/machine-id
'

log "chown → $USER so bwrap/assets server run unprivileged"
sudo chown -R "$(id -u):$(id -g)" "$ROOTFS_DIR"

log "rootfs summary"
du -sh "$ROOTFS_DIR"
rootfs_run -- sh -c 'echo "wine: $(command -v wine64 || command -v wine) $(wine --version 2>/dev/null || wine64 --version)"; Xvfb -version 2>&1 | head -1; busybox | head -1; /opt/rl/bin/hello-static; /opt/rl/bin/hello-dyn'
