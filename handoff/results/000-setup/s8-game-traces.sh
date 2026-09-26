#!/bin/sh
# s8-game-traces.sh — LAPTOP: strace references E1 (startup + first 60 s of the
# main menu) and E2 (first 60 s of an exhibition match vs bots) in ONE game
# session. REPO's laptop/refs/guest/e1.sh/e2.sh launch without -noeac/-EpicPortal
# and with network, which quits or hangs (see results/000-setup/verdict.md, S7),
# so this uses the S7 known-good launch: no network, GPU (ANV, MESA_VK_WSI_DEBUG=sw),
# RocketLeague.exe -nomovie -noeac + legendary's offline Epic args.
# LAPTOP steers the menus from outside (s7-game.sh xdo/shot) and drops trigger
# files into $REFS_DIR/out/<name>/:
#   out/e1/menu.go   main menu reached → trace 60 s more, then detach (E1 done)
#   out/e2/match.go  kickoff → attach strace to game + wineserver for 60 s (E2)
# Then quit the game (Alt+F4); the sandbox ends when the game exits.
# Traces: $REFS_DIR/e1.trace(.gz), e2.trace(.gz) — strace -f -tt -y -s 160 (as refs/run.sh).
set -eu
REPO_ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
set -a; . "$REPO_ROOT/laptop/config.env"; set +a
XDIR=$HOME/rltvos/run/x11; mkdir -p "$XDIR"; chmod 1777 "$XDIR"
mkdir -p "$REFS_DIR/out/e1" "$REFS_DIR/out/e2"
rm -f "$REFS_DIR/e1.trace" "$REFS_DIR/e2.trace" "$REFS_DIR/out/e1/menu.go" "$REFS_DIR/out/e2/match.go"
exec "$REPO_ROOT/laptop/rootfs_exec.sh" --bind "$XDIR" /tmp/.X11-unix --nonet --gpu \
  --env MESA_VK_WSI_DEBUG=sw --env WINEDEBUG=-all --env DXVK_LOG_LEVEL=info \
  --env "WINEDLLOVERRIDES=mscoree,mshtml,winemenubuilder.exe=d" -- sh -c '
say() { echo "[$(date +%T)] $*"; }
Xvfb :99 -screen 0 1280x720x24 -nolisten tcp -extension MIT-SHM -noreset >/refs/out/e1/xvfb.log 2>&1 &
export DISPLAY=:99
i=0; until xdpyinfo >/dev/null 2>&1; do i=$((i+1)); [ $i -lt 100 ] || exit 1; sleep 0.1; done
cd /game/Binaries/Win64
EPIC="-AUTH_LOGIN=unused -AUTH_PASSWORD=0 -AUTH_TYPE=exchangecode -epicapp=Sugar -epicenv=Prod -EpicPortal -epiclocale=en -epicsandboxid=9773aa1aa54f4f7b80e44bef04986cea"
say "E1: strace from launch (wineserver -f -p first, as on the TV)"
strace -f -tt -y -s 160 -o /refs/e1.trace sh -c "wineserver -f -p & sleep 0.5; exec wine /game/Binaries/Win64/RocketLeague.exe -nomovie -noeac $EPIC" >/refs/out/e1/game.log 2>&1 &
ST=$!
until [ -f /refs/out/e1/menu.go ]; do sleep 1; kill -0 $ST 2>/dev/null || { say "game ended before the menu"; exit 1; }; done
say "E1: main menu reached, tracing 60 s more"; sleep 60
kill -TERM $ST 2>/dev/null; sleep 2; say "E1: strace detached"
until [ -f /refs/out/e2/match.go ]; do sleep 1; pgrep -f "RocketLeague[.]exe" >/dev/null || { say "game exited before E2"; exit 1; }; done
P="$(pgrep -f "RocketLeague[.]exe" | sed "s/^/-p /" | tr "\n" " ") $(pgrep -x wineserver | sed "s/^/-p /" | tr "\n" " ")"
say "E2: attaching strace to $P for 60 s"
timeout -s INT 60 strace -f -tt -y -s 160 -o /refs/e2.trace $P || true
say "E2: done; waiting for the game to exit (Alt+F4)"
while pgrep -f "RocketLeague[.]exe" >/dev/null; do sleep 2; done
wineserver -k 2>/dev/null || true
say "all done"
'
