#!/bin/sh
# install.sh — LAPTOP's INSTALL_CMD (laptop/config.env): sideload an IPA onto the
# Apple TV through atvloadly's MCP endpoint. usage: laptop/install.sh path/to/app.ipa
exec python3 "$(dirname "$0")/atvloadly.py" install "$@"
