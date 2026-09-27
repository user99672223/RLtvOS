#!/bin/sh
# C5 — wine64 notepad under Xvfb: wineserver -f -p, wineboot chain
# (services.exe, winedevice, plugplay, rpcss, explorer), /proc/self/*,
# /proc/<pid>/mem, futex, eventfd, memfd_create, /dev/shm, tgkill.
. "$(dirname "$0")/_lib.sh"
start_xvfb :0 || exit 1
wine_env
start_wineserver
W=$(wine_bin)
say "wine: $W $($W --version 2>/dev/null)"
$W notepad >"$OUT/notepad.log" 2>&1 &
NP=$!
i=0
while [ $i -lt "${RL_WAIT:-20}" ]; do
  sleep 1; i=$((i + 1))
  if xdotool search --name -i notepad >/dev/null 2>&1; then say "notepad window after ${i}s"; break; fi
done
xwininfo -root -tree | grep -i -E "notepad|wine" | head -5
snap c5
$W taskkill /im notepad.exe /f >/dev/null 2>&1 || kill $NP 2>/dev/null
sleep 1
stop_wine
stop_xvfb
echo "C5 done"
