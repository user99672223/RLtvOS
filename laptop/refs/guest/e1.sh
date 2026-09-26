#!/bin/sh
# E1 — RocketLeague.exe (no EAC, 720p, low settings, 30 fps cap, -nomovie)
# reaches the main menu. RL_USE_REAL_DISPLAY=1 keeps the caller's DISPLAY
# (laptop reference with hardware Vulkan); otherwise Xvfb :0 like the TV.
. /refs/guest/_lib.sh
if [ "${RL_USE_REAL_DISPLAY:-0}" = 1 ] && [ -n "${DISPLAY:-}" ]; then
  say "using real display $DISPLAY"
else
  start_xvfb :0 || exit 1
fi
wine_env
start_wineserver
W=$(wine_bin)
cd /game/Binaries/Win64 || { echo "no /game"; exit 1; }
[ -d EasyAntiCheat ] && say "EAC dir present, not used"
say "launching RocketLeague.exe"
$W RocketLeague.exe -nomovie -nosplash -windowed -ResX=1280 -ResY=720 >"$OUT/e1.game.log" 2>&1 &
GP=$!
i=0
while [ $i -lt "${RL_WAIT:-180}" ]; do
  sleep 1; i=$((i + 1))
  [ $((i % 30)) -eq 0 ] && { snap "e1-$i"; say "t=${i}s"; }
  kill -0 $GP 2>/dev/null || { say "game process exited at ${i}s"; break; }
done
snap e1
tail -20 "$OUT/e1.game.log"
$W taskkill /im RocketLeague.exe /f >/dev/null 2>&1 || true
sleep 2
stop_wine
stop_xvfb
echo "E1 done"
