#!/bin/sh
# s6-prefix.sh — how LAPTOP created the Wine prefix (S6): 64-bit prefix under
# Xvfb (MIT-SHM off, as on the TV), then Gcenx DXVK-macOS (d3d11 + d3d10core
# only; that build ships no dxgi/d3d9 — Wine's builtin dxgi is used).
# Runs inside the rootfs via laptop/rootfs_exec.sh (bwrap, no root).
set -eu
REPO_ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
set -a; . "$REPO_ROOT/laptop/config.env"; set +a
DXVK_DIR=$HOME/rltvos/build/dxvk/dxvk-macOS-async-$DXVK_MACOS_VERSION
[ -f "$DXVK_DIR/x64/d3d11.dll" ] || { echo "missing $DXVK_DIR" >&2; exit 1; }
"$REPO_ROOT/laptop/rootfs_exec.sh" --ro-bind "$DXVK_DIR" /tmp/dxvk -- sh -c '
set -eu
Xvfb :99 -screen 0 1280x720x24 -nolisten tcp -extension MIT-SHM >/tmp/xvfb.log 2>&1 &
export DISPLAY=:99
i=0; until xdpyinfo >/dev/null 2>&1; do i=$((i+1)); [ $i -lt 100 ] || { cat /tmp/xvfb.log; exit 1; }; sleep 0.1; done
export WINEDEBUG=-all WINEDLLOVERRIDES="winemenubuilder.exe=d;mscoree=d;mshtml=d"
echo "== wineboot -i"; wine wineboot -i; wineserver -w
echo "== DXVK"
cp /tmp/dxvk/x64/d3d11.dll /tmp/dxvk/x64/d3d10core.dll /prefix/drive_c/windows/system32/
for d in d3d11 d3d10core; do wine reg add "HKCU\\Software\\Wine\\DllOverrides" /v $d /d native /f; done
wine reg add "HKCU\\Software\\Wine\\DllOverrides" /v winemenubuilder.exe /d "" /f
wineserver -w
echo "== result"; wine --version; wine reg query "HKCU\\Software\\Wine\\DllOverrides"
ls -la /prefix /prefix/drive_c/windows/system32/d3d11.dll /prefix/drive_c/windows/system32/dxgi.dll
'
du -sh "$WINEPREFIX_DIR"
