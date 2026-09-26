#!/bin/sh
# E2 — exhibition match vs bots. Launches like E1, waits for the menu, then
# replays /refs/guest/e2-keys.txt through xdotool (LAPTOP validates the key
# sequence on the laptop first and records the working one in the result).
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
$W RocketLeague.exe -nomovie -nosplash -windowed -ResX=1280 -ResY=720 >"$OUT/e2.game.log" 2>&1 &
GP=$!
say "waiting ${RL_MENU_WAIT:-90}s for the main menu"
sleep "${RL_MENU_WAIT:-90}"
snap e2-menu
KEYS=${RL_E2_KEYS:-/refs/guest/e2-keys.txt}
say "replaying $KEYS"
while IFS= read -r line; do
  case "$line" in ''|'#'*) continue ;; esac
  set -- $line
  case "$1" in
    key)   shift; xdotool key --delay 120 "$@" ;;
    type)  shift; xdotool type --delay 80 "$*" ;;
    sleep) sleep "$2" ;;
    snap)  snap "$2" ;;
    mouse) xdotool mousemove "$2" "$3" click 1 ;;
    *)     say "unknown directive: $line" ;;
  esac
done <"$KEYS"
say "match should be running; sampling ${RL_MATCH_WAIT:-120}s"
i=0
while [ $i -lt "${RL_MATCH_WAIT:-120}" ]; do
  sleep 1; i=$((i + 1))
  [ $((i % 20)) -eq 0 ] && snap "e2-match-$i"
  kill -0 $GP 2>/dev/null || { say "game process exited at ${i}s"; break; }
done
snap e2
tail -20 "$OUT/e2.game.log"
$W taskkill /im RocketLeague.exe /f >/dev/null 2>&1 || true
sleep 2
stop_wine
stop_xvfb
echo "E2 done"
