#!/bin/sh
# C4 — Xvfb :0 + xdpyinfo + xeyes: unix sockets, poll/select, SO_PEERCRED,
# fcntl locks (Xvfb's /tmp/.X0-lock), XTEST via xdotool.
. /refs/guest/_lib.sh
start_xvfb :0 || exit 1
xdpyinfo -display :0 | head -30
xeyes -display :0 -geometry 400x300+100+100 &
XE=$!
sleep 2
xwininfo -root -tree -display :0 | head -15
xdotool mousemove 300 200 sleep 0.3 mousemove 700 500 sleep 0.3 mousemove 200 600 || echo "xdotool failed"
sleep 0.5
snap c4
kill $XE 2>/dev/null
stop_xvfb
echo "C4 done"
