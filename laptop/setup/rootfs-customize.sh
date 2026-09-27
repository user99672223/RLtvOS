#!/bin/sh
# rootfs-customize.sh — runs INSIDE the rootfs (as root or fake root) after
# the packages are installed: builds the checkpoint binaries into /opt/rl/bin
# and writes the minimal /etc the guest needs. Expects the sources in
# /opt/rl/src (copied in by 10-rootfs.sh). Idempotent.
set -e
export PATH=/opt/wine-stable/bin:/opt/wine-devel/bin:/opt/wine-staging/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
mkdir -p /opt/rl/bin /opt/rl/src
cd /opt/rl/src

# Every guest executable must be PIE: the TV cannot map anything below 4 GB
# (XNU enforces a 4 GB hard page zero on arm64 processes), so an ET_EXEC
# linked at 0x400000 can never run there (handoff/DECISIONS.md 2026-09-27).
# -static-pie works without startup code here: hello_static.c has no
# relocations to apply (RIP-relative addressing only).
if ! gcc -static-pie -fPIE -nostdlib -nostartfiles -O2 -o /opt/rl/bin/hello-static hello_static.c; then
  echo "WARN: -static-pie failed; building a non-PIE hello-static (will NOT run on the TV)"
  gcc -static -nostdlib -nostartfiles -O2 -o /opt/rl/bin/hello-static hello_static.c
fi
file /opt/rl/bin/hello-static
case "$(file -b /opt/rl/bin/hello-static)" in
  *"pie executable"*|*"shared object"*) echo "hello-static: PIE ok" ;;
  *) echo "WARN: hello-static is not PIE" ;;
esac
gcc -O2 -o /opt/rl/bin/hello-dyn hello_dyn.c
gcc -O2 -pthread -o /opt/rl/bin/threads-test threads_test.c
if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
  x86_64-w64-mingw32-gcc -O2 -municode -o /opt/rl/bin/d3d11tri.exe d3d11tri.c \
    -ld3d11 -ldxgi -ld3dcompiler -ldxguid -luuid -luser32 -lgdi32 || echo "WARN: d3d11tri build failed"
else
  echo "WARN: mingw-w64 missing; d3d11tri.exe not built"
fi
ls -la /opt/rl/bin

grep -q "^user:" /etc/passwd || echo "user:x:1000:1000:user:/home/user:/bin/sh" >> /etc/passwd
grep -q "^user:" /etc/group || echo "user:x:1000:" >> /etc/group
mkdir -p /home/user /prefix /game /refs /tmp/.X11-unix
chmod 1777 /tmp /tmp/.X11-unix || true
echo rltvos > /etc/hostname
printf "127.0.0.1 localhost\n127.0.1.1 rltvos\n" > /etc/hosts
[ -s /etc/machine-id ] || echo 9d3b1c1a2e4f4c0aa1f5b2c3d4e5f607 > /etc/machine-id
# Wine looks for these; keep them deterministic for the manifest.
mkdir -p /etc/fonts /var/tmp
chmod 1777 /var/tmp || true
apt-get clean 2>/dev/null || true
rm -rf /var/lib/apt/lists/* /var/cache/apt/*.bin 2>/dev/null || true
echo "customize done: $(command -v wine64 || command -v wine || echo 'wine MISSING')"
