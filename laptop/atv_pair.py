#!/usr/bin/env python3
"""atv_pair.py — LAPTOP helper: pair pyatv with the Apple TV without a TTY.

The TV shows a PIN; the user reads it out in chat and LAPTOP writes it to the
pin file. Credentials land in pyatv's default file storage (~/.pyatv.conf), the
same storage `atvremote` uses, so tools/tv.py needs no extra arguments.

    atv_pair.py --protocol companion|airplay [--id ID] [--host IP] [--pin-file F] [--timeout S]

Prints one JSON line per state change: pin_requested, paired / error.
"""
import argparse
import asyncio
import json
import os
import pathlib
import sys
import time

import pyatv
from pyatv.const import Protocol
from pyatv.storage.file_storage import FileStorage


def emit(**kw):
    print(json.dumps(kw), flush=True)


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--protocol", required=True, choices=["companion", "airplay"])
    ap.add_argument("--id", default=os.environ.get("TV_DEVICE_ID"))
    ap.add_argument("--host", default=os.environ.get("TV_IP"))
    ap.add_argument("--pin-file", default="/tmp/atv_pair.pin")
    ap.add_argument("--timeout", type=int, default=300)
    a = ap.parse_args()

    loop = asyncio.get_running_loop()
    storage = FileStorage.default_storage(loop)
    await storage.load()
    hosts = [a.host] if a.host else None
    confs = await pyatv.scan(loop, identifier=a.id, hosts=hosts, storage=storage, timeout=5)
    if not confs:
        emit(state="error", error=f"device not found (id={a.id} host={a.host})")
        return 2
    conf = confs[0]
    proto = Protocol.Companion if a.protocol == "companion" else Protocol.AirPlay
    pin_file = pathlib.Path(a.pin_file)
    pin_file.unlink(missing_ok=True)

    pairing = await pyatv.pair(conf, proto, loop, storage=storage, name="rltvos-laptop")
    try:
        await pairing.begin()
        if not pairing.device_provides_pin:
            emit(state="error", error="device does not provide a PIN for this protocol")
            return 3
        emit(state="pin_requested", protocol=a.protocol, device=conf.name, pin_file=str(pin_file))
        deadline = time.time() + a.timeout
        while not pin_file.exists():
            if time.time() > deadline:
                emit(state="error", error="timed out waiting for PIN")
                return 4
            await asyncio.sleep(0.5)
        pin = pin_file.read_text().strip()
        pin_file.unlink(missing_ok=True)
        pairing.pin(int(pin))
        await pairing.finish()
        if pairing.has_paired:
            await storage.save()
            emit(state="paired", protocol=a.protocol, device=conf.name, storage=str(storage))
            return 0
        emit(state="error", error="pairing did not complete (wrong PIN?)")
        return 5
    finally:
        await pairing.close()


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
