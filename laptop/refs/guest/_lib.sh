#!/bin/sh
# _lib.sh — sourced by the checkpoint scripts. Runs INSIDE the guest: natively
# in the rootfs (bwrap on the laptop, laptop/refs/run.sh) and later on the TV
# under the fake kernel. Keep it POSIX sh and busybox-compatible.
OUT=${RL_OUT:-/refs/out}
mkdir -p "$OUT" 2>/dev/null || OUT=/tmp
say() { echo "[$(date +%T 2>/dev/null || echo ?)] $*"; }

# start_xvfb [:N]  — Xvfb the way the TV runs it: no MIT-SHM, no TCP.
start_xvfb() {
  disp=${1:-:0}
  Xvfb "$disp" -screen 0 "${RL_SCREEN:-1280x720x24}" -nolisten tcp -extension MIT-SHM -noreset \
    >"$OUT/xvfb.log" 2>&1 &
  XVFB_PID=$!
  export DISPLAY="$disp"
  i=0
  while [ $i -lt 100 ]; do
    [ -S "/tmp/.X11-unix/X${disp#:}" ] && { say "Xvfb up on $disp (pid $XVFB_PID)"; return 0; }
    i=$((i + 1)); sleep 0.1
  done
  say "Xvfb did not start; log:"; cat "$OUT/xvfb.log"
  return 1
}
stop_xvfb() { [ -n "${XVFB_PID:-}" ] && kill "$XVFB_PID" 2>/dev/null; return 0; }

wine_bin() { command -v wine64 2>/dev/null || command -v wine; }

# Wine environment shared by C5/D2/E1/E2. wineserver is started explicitly
# (-f foreground, -p persistent) so wine64 connects instead of forking it.
wine_env() {
  export WINEPREFIX=${WINEPREFIX:-/prefix}
  export WINEARCH=win64
  export WINEDEBUG=${WINEDEBUG:--all}
  export WINEDLLOVERRIDES=${WINEDLLOVERRIDES:-"mscoree,mshtml,winemenubuilder.exe=d"}
  export DXVK_CONFIG_FILE=${DXVK_CONFIG_FILE:-'C:\dxvk.conf'}
  export DXVK_LOG_PATH=${DXVK_LOG_PATH:-$OUT}
  export DXVK_LOG_LEVEL=${DXVK_LOG_LEVEL:-info}
  export DXVK_STATE_CACHE_PATH=${DXVK_STATE_CACHE_PATH:-/home/user/dxvk-cache}
  export DXVK_HUD=${DXVK_HUD:-fps,version,api}
  mkdir -p "$DXVK_STATE_CACHE_PATH" 2>/dev/null || true
}
start_wineserver() {
  wineserver -f -p >"$OUT/wineserver.log" 2>&1 &
  WS_PID=$!
  sleep 0.5
  say "wineserver pid $WS_PID"
}
stop_wine() { wineserver -k 2>/dev/null; sleep 1; wineserver -k9 2>/dev/null; return 0; }

# snap NAME — dump the X root window (xwd); run.sh converts it to PNG.
snap() {
  if command -v xwd >/dev/null 2>&1 && [ -n "${DISPLAY:-}" ]; then
    xwd -root -silent -display "$DISPLAY" >"$OUT/$1.xwd" 2>/dev/null && say "snapshot $OUT/$1.xwd"
  fi
  return 0
}
