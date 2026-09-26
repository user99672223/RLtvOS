#!/bin/sh
# s7-game.sh — LAPTOP's S7 known-good driver: Rocket League offline, no EAC,
# under Xvfb + WineHQ wine (64-bit) + DXVK-macOS, hardware Vulkan (ANV) with
# Mesa's software presentation (Xvfb has no DRI3).
#   start [extra game args]   Xvfb :99 + game, in the background (blocks until the game exits)
#   shot NAME                 PNG of the X root window → $OUT/shots/NAME.png
#   xdo ARGS...               xdotool against :99 (key/mousemove/click/type)
#   stop                      kill the game sandbox
# NONET=1 runs the game with no network (Rocket League then goes offline instead
# of waiting on the title screen for an Epic session).
# VDESK=WxH runs the game inside a Wine virtual desktop (explorer /desktop=RL,WxH):
# with no window manager on Xvfb, XTEST input only reaches the game this way.
# The game is launched as `legendary launch Sugar --offline --override-exe
# Binaries/Win64/RocketLeague.exe -nomovie -noeac` would (-noeac = Epic's own
# "Launch without Anti-Cheat" option; Launcher.exe is only a .NET wrapper that
# picks RocketLeague_EAC.exe or RocketLeague.exe). legendary --offline passes
# -AUTH_PASSWORD=0 and the -EpicPortal args; without them EOS's LauncherCheck
# makes the game quit. -epicusername/-epicuserid are omitted (this laptop's
# legendary has no Epic login, which is also why --dry-run itself fails).
set -eu
REPO_ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
RUN="$REPO_ROOT/laptop/rootfs_exec.sh"
OUT=${OUT:-$HOME/rltvos/build/s7}
mkdir -p "$OUT/shots"
# The X socket dir is shared through a host dir so helpers can reach Xvfb even
# when the game runs without network (NONET=1 → own netns, no abstract socket).
XDIR=$HOME/rltvos/run/x11; mkdir -p "$XDIR"; chmod 1777 "$XDIR"
X="--bind $XDIR /tmp/.X11-unix"
case "${1:-}" in
start)
  shift
  NET=""; [ "${NONET:-0}" = 1 ] && NET=--nonet
  exec "$RUN" $X $NET --gpu --env MESA_VK_WSI_DEBUG=sw --env DXVK_LOG_LEVEL=info \
    --env "WINEDEBUG=${WINEDEBUG:-fixme-all}" --env DXVK_HUD=fps --env "VDESK=${VDESK:-}" -- sh -c '
Xvfb :99 -screen 0 1280x720x24 -nolisten tcp -extension MIT-SHM >/tmp/xvfb.log 2>&1 &
export DISPLAY=:99
i=0; until xdpyinfo >/dev/null 2>&1; do i=$((i+1)); [ $i -lt 100 ] || exit 1; sleep 0.1; done
cd /game/Binaries/Win64
EPIC="-AUTH_LOGIN=unused -AUTH_PASSWORD=0 -AUTH_TYPE=exchangecode -epicapp=Sugar -epicenv=Prod -EpicPortal -epiclocale=en -epicsandboxid=9773aa1aa54f4f7b80e44bef04986cea"
echo "[$(date +%T)] launch: wine ${VDESK:+explorer /desktop=RL,$VDESK }/game/Binaries/Win64/RocketLeague.exe -nomovie -noeac $EPIC $*"
PRE=""; [ -z "$VDESK" ] || PRE="explorer /desktop=RL,$VDESK"
wine $PRE /game/Binaries/Win64/RocketLeague.exe -nomovie -noeac $EPIC "$@"
echo "[$(date +%T)] game exited rc=$?"
' s7 "$@" ;;
shot)
  "$RUN" $X --bind "$OUT/shots" /tmp/shots -- sh -c "DISPLAY=:99 import -window root /tmp/shots/$2.png" && echo "$OUT/shots/$2.png" ;;
xdo)
  shift; "$RUN" $X -- env DISPLAY=:99 xdotool "$@" ;;
stop)
  pkill -f '[b]wrap .* MESA_VK_WSI_DEBUG sw' || true ;;
*) echo "usage: $0 start|shot NAME|xdo ARGS|stop" >&2; exit 2 ;;
esac
