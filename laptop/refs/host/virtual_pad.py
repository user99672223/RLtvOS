#!/usr/bin/env python3
"""virtual_pad.py SECONDS — create a uinput gamepad (Xbox-style layout) and
press buttons / move sticks in a fixed pattern for SECONDS. Needs write
access to /dev/uinput (run.sh uses sudo). The guest's evtest (D3) and, later,
Wine's winebus/SDL see it as a normal evdev joystick.
"""
import sys
import time

try:
    from evdev import AbsInfo, UInput, ecodes as e
except ImportError:
    print("python3-evdev missing (apt install python3-evdev)", file=sys.stderr)
    sys.exit(2)

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0
absinfo = AbsInfo(value=0, min=-32768, max=32767, fuzz=16, flat=128, resolution=0)
trig = AbsInfo(value=0, min=0, max=1023, fuzz=0, flat=0, resolution=0)
hat = AbsInfo(value=0, min=-1, max=1, fuzz=0, flat=0, resolution=0)
cap = {
    e.EV_KEY: [e.BTN_SOUTH, e.BTN_EAST, e.BTN_NORTH, e.BTN_WEST, e.BTN_TL, e.BTN_TR,
               e.BTN_SELECT, e.BTN_START, e.BTN_MODE, e.BTN_THUMBL, e.BTN_THUMBR],
    e.EV_ABS: [(e.ABS_X, absinfo), (e.ABS_Y, absinfo), (e.ABS_RX, absinfo), (e.ABS_RY, absinfo),
               (e.ABS_Z, trig), (e.ABS_RZ, trig), (e.ABS_HAT0X, hat), (e.ABS_HAT0Y, hat)],
}
ui = UInput(cap, name="RLtvOS Virtual Pad", vendor=0x045e, product=0x028e, version=0x0110, bustype=e.BUS_USB)
print(f"created {ui.device.path} ({ui.name}) for {secs}s", flush=True)
t0 = time.time()
i = 0
buttons = [e.BTN_SOUTH, e.BTN_EAST, e.BTN_NORTH, e.BTN_WEST, e.BTN_START]
try:
    while time.time() - t0 < secs:
        b = buttons[i % len(buttons)]
        ui.write(e.EV_KEY, b, 1); ui.syn(); time.sleep(0.15)
        ui.write(e.EV_KEY, b, 0); ui.syn()
        x = int(32767 * ((i % 8) / 4.0 - 1.0))
        ui.write(e.EV_ABS, e.ABS_X, x)
        ui.write(e.EV_ABS, e.ABS_RZ, (i * 97) % 1024)
        ui.syn()
        time.sleep(0.35)
        i += 1
finally:
    ui.close()
print(f"done after {i} iterations", flush=True)
