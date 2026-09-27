#!/bin/sh
# D2 — DXVK d3d11 sample under wine64 (d3d11tri.exe, built by 10-rootfs.sh).
. "$(dirname "$0")/_lib.sh"
start_xvfb :0 || exit 1
wine_env
start_wineserver
W=$(wine_bin)
export RL_D3D_FRAMES=${RL_FRAMES:-300}
$W /opt/rl/bin/d3d11tri.exe >"$OUT/d3d11tri.log" 2>&1 &
TP=$!
sleep 6
xwininfo -root -tree | grep -i d3d11tri | head -3
snap d2
wait $TP
echo "d3d11tri exit=$?"
tail -5 "$OUT/d3d11tri.log"
ls "$OUT"/*.log 2>/dev/null | grep -i dxvk | head -3
stop_wine
stop_xvfb
echo "D2 done"
