#!/bin/sh
# D3 — evdev device visible to evtest. On the laptop run.sh starts a uinput
# virtual gamepad (refs/host/virtual_pad.py) and binds /dev/input; on the TV
# the fake kernel synthesises /dev/input/eventN from GameController.framework.
. "$(dirname "$0")/_lib.sh"
ls -l /dev/input 2>/dev/null || echo "no /dev/input"
DEV=${RL_EVDEV:-}
if [ -z "$DEV" ]; then
  for d in /dev/input/event*; do
    [ -e "$d" ] || continue
    if timeout 1 evtest --query "$d" EV_KEY BTN_SOUTH >/dev/null 2>&1 || true; then DEV=$d; fi
  done
fi
[ -n "$DEV" ] || DEV=/dev/input/event0
say "reading $DEV for ${RL_WAIT:-8}s"
timeout "${RL_WAIT:-8}" evtest "$DEV" 2>&1 | head -80
echo "D3 done"
