#!/bin/sh
# 00-prereqs.sh — host packages and Python tools the laptop side needs.
# Idempotent. Needs sudo for apt.
set -eu
. "$(dirname "$0")/lib.sh"

need sudo
log "apt packages"
sudo apt-get update -qq
sudo apt-get install -y --no-install-recommends \
  debootstrap bubblewrap strace gzip xz-utils zip unzip curl ca-certificates gnupg jq file \
  python3 python3-pip python3-venv python3-evdev \
  binutils gcc libc6-dev \
  x11-apps x11-utils xdotool imagemagick \
  mingw-w64 \
  libvulkan1 vulkan-tools mesa-vulkan-drivers

# pyatv (atvremote) in a venv on PATH; pipx if available.
if ! have atvremote; then
  log "installing pyatv (atvremote)"
  if have pipx; then pipx install pyatv
  else
    python3 -m venv "$HOME/.local/share/pyatv-venv"
    "$HOME/.local/share/pyatv-venv/bin/pip" install -q pyatv
    mkdir -p "$HOME/.local/bin"
    ln -sf "$HOME/.local/share/pyatv-venv/bin/atvremote" "$HOME/.local/bin/atvremote"
    log "atvremote linked into ~/.local/bin (make sure it is on PATH)"
  fi
fi

# Unprivileged user namespaces are needed by bwrap on some Debian kernels.
if [ "$(sysctl -n kernel.unprivileged_userns_clone 2>/dev/null || echo 1)" = 0 ]; then
  log "enabling kernel.unprivileged_userns_clone"
  echo 'kernel.unprivileged_userns_clone=1' | sudo tee /etc/sysctl.d/90-rltvos-userns.conf >/dev/null
  sudo sysctl -q -p /etc/sysctl.d/90-rltvos-userns.conf
fi

# uinput access for the D3 virtual controller (evtest reference).
if [ ! -e /dev/uinput ]; then sudo modprobe uinput || true; fi

mkdir -p "$ASSETS_DIR" "$REFS_DIR" "$OUT_DIR"
if [ ! -f "$CONFIG_ENV" ]; then
  cp "$REPO_ROOT/laptop/config.env.example" "$CONFIG_ENV"
  log "created $CONFIG_ENV from the example — fill in TV_IP, TV_DEVICE_ID, JIT_CMD, INSTALL_CMD"
fi
log "prereqs done. Versions:"
bwrap --version; strace -V | head -1; python3 --version; atvremote --version 2>/dev/null || true
