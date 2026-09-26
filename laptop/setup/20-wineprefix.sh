#!/bin/sh
# 20-wineprefix.sh — create the 64-bit Wine prefix at $WINEPREFIX_DIR (seen as
# /prefix inside the guest), install Gcenx's dxvk-macOS DLLs, and apply the
# registry settings the TV run needs (no audio driver, no crash dialog, no
# winemenubuilder, DXVK overrides). Runs inside the rootfs via bwrap under
# its own Xvfb, exactly as the TV will. Idempotent.
set -eu
. "$(dirname "$0")/lib.sh"
need bwrap; need curl; need tar

mkdir -p "$WINEPREFIX_DIR" "$HOME_DIR" "$ASSETS_DIR/downloads"

# --- DXVK (Gcenx dxvk-macOS: built against MoltenVK's feature set) ----------
DL="$ASSETS_DIR/downloads"
if [ -z "${DXVK_MACOS_VERSION:-}" ]; then
  DXVK_MACOS_VERSION=$(curl -fsSL https://api.github.com/repos/Gcenx/DXVK-macOS/releases/latest | python3 -c 'import json,sys; print(json.load(sys.stdin)["tag_name"])')
fi
log "dxvk-macOS version: $DXVK_MACOS_VERSION"
if [ ! -d "$DL/dxvk-macOS-$DXVK_MACOS_VERSION" ]; then
  url=$(curl -fsSL "https://api.github.com/repos/Gcenx/DXVK-macOS/releases/tags/$DXVK_MACOS_VERSION" | python3 -c '
import json,sys
for a in json.load(sys.stdin)["assets"]:
    n=a["name"]
    if n.endswith((".tar.gz",".tar.xz",".tar.zst",".zip")) and "async" not in n.lower(): print(a["browser_download_url"]); break')
  [ -n "$url" ] || die "no dxvk-macOS asset found for $DXVK_MACOS_VERSION"
  log "downloading $url"
  f="$DL/$(basename "$url")"
  curl -fL -o "$f" "$url"
  mkdir -p "$DL/dxvk-macOS-$DXVK_MACOS_VERSION"
  case "$f" in
    *.zip) unzip -q -o "$f" -d "$DL/dxvk-macOS-$DXVK_MACOS_VERSION" ;;
    *) tar -xf "$f" -C "$DL/dxvk-macOS-$DXVK_MACOS_VERSION" ;;
  esac
fi
DXVK_X64=$(find "$DL/dxvk-macOS-$DXVK_MACOS_VERSION" -type d -name x64 | head -1)
[ -n "$DXVK_X64" ] || die "x64 directory not found in the dxvk-macOS archive"
echo "$DXVK_MACOS_VERSION" > "$ASSETS_DIR/dxvk-version.txt"

# --- wineboot inside the rootfs ------------------------------------------------
log "wineboot (64-bit prefix at /prefix)"
rootfs_run --env WINEDLLOVERRIDES="mscoree,mshtml,winemenubuilder.exe=d" --env WINEDEBUG=-all -- sh -c '
set -e
Xvfb :9 -screen 0 1280x720x24 -nolisten tcp -extension MIT-SHM >/tmp/xvfb.log 2>&1 &
XPID=$!
export DISPLAY=:9
for i in $(seq 1 50); do [ -S /tmp/.X11-unix/X9 ] && break; sleep 0.1; done
WINE=$(command -v wine64 || command -v wine)
echo "using $WINE ($($WINE --version))"
$WINE wineboot -u
$WINE reg add "HKCU\\Software\\Wine\\Drivers" /v Audio /t REG_SZ /d "" /f
$WINE reg add "HKCU\\Software\\Wine\\WineDbg" /v ShowCrashDialog /t REG_DWORD /d 0 /f
$WINE reg add "HKCU\\Software\\Wine\\DllOverrides" /v winemenubuilder.exe /t REG_SZ /d "" /f
$WINE reg add "HKCU\\Software\\Wine\\DllOverrides" /v d3d11 /t REG_SZ /d native /f
$WINE reg add "HKCU\\Software\\Wine\\DllOverrides" /v d3d10core /t REG_SZ /d native /f
# dxgi: DXVK-macOS ships no dxgi.dll; Wine's builtin dxgi works with it (LAPTOP S6).
$WINE reg add "HKCU\\Software\\Wine\\Direct3D" /v renderer /t REG_SZ /d vulkan /f
$WINE reg add "HKCU\\Software\\Wine\\X11 Driver" /v Decorated /t REG_SZ /d N /f
$WINE reg add "HKCU\\Software\\Wine\\X11 Driver" /v Managed /t REG_SZ /d N /f
$WINE reg add "HKCU\\Software\\Wine\\X11 Driver" /v GrabFullscreen /t REG_SZ /d Y /f
$WINE reg add "HKCU\\Software\\Wine\\X11 Driver" /v UseXVidMode /t REG_SZ /d N /f
$WINE reg add "HKCU\\Software\\Wine\\X11 Driver" /v UseXRandR /t REG_SZ /d N /f
wineserver -w
kill $XPID 2>/dev/null || true
'

log "installing DXVK DLLs into system32"
for dll in d3d11 dxgi d3d10core; do
  if [ -f "$DXVK_X64/$dll.dll" ]; then
    cp -f "$DXVK_X64/$dll.dll" "$WINEPREFIX_DIR/drive_c/windows/system32/$dll.dll"
    if [ "$dll" = dxgi ]; then
      rootfs_run -- sh -c '$(command -v wine64 || command -v wine) reg add "HKCU\\Software\\Wine\\DllOverrides" /v dxgi /t REG_SZ /d native /f; wineserver -w' || true
    fi
  else
    log "note: $dll.dll not in $DXVK_X64 (DXVK-macOS ships d3d11/d3d10core only; Wine's builtin $dll is used)"
  fi
done

# DXVK config: conservative, matches the TV's MoltenVK feature level.
cat > "$WINEPREFIX_DIR/drive_c/dxvk.conf" <<'EOF'
# dxvk.conf (guest sees this through DXVK_CONFIG_FILE=C:\dxvk.conf)
dxgi.maxFrameRate = 30
d3d11.maxFrameRate = 30
dxvk.numCompilerThreads = 2
dxgi.syncInterval = 1
d3d11.maxFeatureLevel = 11_0
d3d11.maxTessFactor = 8
dxgi.maxDeviceMemory = 1024
dxgi.maxSharedMemory = 1024
d3d11.samplerAnisotropy = 1
EOF

# User docs dir the game writes its config into; pre-seeded by 30-game.sh.
mkdir -p "$WINEPREFIX_DIR/drive_c/users/user/Documents/My Games/Rocket League/TAGame/Config"
log "prefix ready: $(du -sh "$WINEPREFIX_DIR" | cut -f1)"
ls "$WINEPREFIX_DIR"
