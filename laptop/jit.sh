#!/bin/sh
# jit.sh — LAPTOP's JIT_CMD (laptop/config.env): prepare JIT memory for the
# RLtvOS app on the Apple TV (tvOS 26+/TXM, StikDebug universal.js protocol,
# see handoff/issues/001-jit.md and 003-jit-txm-brk-protocol.md).
#
#   laptop/jit.sh --pid PID            attach to the running app (tv.py passes {pid})
#   laptop/jit.sh --launch BUNDLE_ID   launch it suspended, then attach
#   laptop/jit.sh --probe              tunnel + developer-service check only
#
# Rootless: laptop/jit/ (rltvos-jit, Rust/idevice) talks RemotePairing over Wi-Fi
# with atvloadly's pairing record; mounts the developer disk image through
# atvloadly first if the debug services are missing. The helper keeps running in
# the background (servicing further PrepareRegion calls) until the app detaches or
# exits; this script returns once the first region is prepared (exit 0) or on
# error/timeout (exit 1). Prints one JSON object.
set -eu
REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
set -a; . "$REPO_ROOT/laptop/config.env"; set +a
BIN=${RLTVOS_JIT_BIN:-$HOME/rltvos/build/jit-target/release/rltvos-jit}
PAIR=${JIT_PAIRING_FILE:-/etc/atvloadly/PlumeImpactor/pairing_files/$TV_UDID.plist}
WAIT=${JIT_WAIT_S:-120}
[ -x "$BIN" ] || { echo "{\"ok\":false,\"error\":\"missing $BIN: cd laptop/jit && CARGO_TARGET_DIR=~/rltvos/build/jit-target cargo build --release\"}"; exit 1; }
[ -r "$PAIR" ] || { echo "{\"ok\":false,\"error\":\"pairing record $PAIR not readable\"}"; exit 1; }

# The TV's _remotepairing._tcp port (atvloadly tracks it); fallback TV_RP_PORT.
PORT=$(curl -fsS -m 10 "$ATVLOADLY_URL/api/devices/$ATVLOADLY_DEVICE_ID" 2>/dev/null | jq -r '.data.port // empty' 2>/dev/null || true)
PORT=${PORT:-${TV_RP_PORT:-49152}}
TV="$TV_IP:$PORT"

probe() { "$BIN" probe --tv "$TV" --pairing "$PAIR" 2>/dev/null | tail -1; }
has_dev() { echo "$1" | grep -q '"com.apple.internal.dt.remote.debugproxy":true'; }

case "${1:-}" in
  --probe) P=$(probe); echo "$P"; has_dev "$P"; exit $? ;;
  --pid) MODE="--pid $2" ;;
  --launch) MODE="--launch $2" ;;
  *) echo '{"ok":false,"error":"usage: jit.sh --pid PID | --launch BUNDLE_ID | --probe"}'; exit 2 ;;
esac

P=$(probe)
if ! has_dev "$P"; then
  # Developer services missing. tvOS 27+: install the Cryptex1 DDI (from Xcode 27, see
  # .github/workflows/laptop-tvos-ddi.yml) via cryptexd; older tvOS: atvloadly's classic mount.
  DDI=${JIT_DDI_DIR:-$HOME/rltvos/ddi/tvos27/published/Xcode_tvOS_DDI_Cryptex}
  if [ -f "$DDI/Image.dmg.cryptex_info" ]; then
    "$BIN" mount-ddi --tv "$TV" --pairing "$PAIR" --ddi "$DDI" >&2 || true
  else
    curl -fsS -m 240 -X POST "$ATVLOADLY_URL/api/devices/$ATVLOADLY_DEVICE_ID/mountimage" >/dev/null 2>&1 || true
  fi
  P=$(probe)
  has_dev "$P" || { echo "{\"ok\":false,\"error\":\"debug services unavailable after mount\",\"probe\":$(printf '%s' "${P:-null}" | head -c 2000)}"; exit 1; }
fi

D=${OUT_DIR:-$HOME/rltvos/out}/jit; mkdir -p "$D"
TS=$(date +%Y%m%d-%H%M%S); ST="$D/status-$TS.json"; LOG="$D/jit-$TS.log"
# setsid + nohup: the helper outlives this script (and tv.py's timeout).
setsid nohup "$BIN" run --tv "$TV" --pairing "$PAIR" $MODE --status "$ST" \
  --timeout "${JIT_HELPER_TIMEOUT:-86400}" >"$LOG" 2>&1 </dev/null &
HP=$!
i=0; ev=""
while [ $i -lt $((WAIT * 2)) ]; do
  ev=$(sed -n 's/.*"event":"\([a-z-]*\)".*/\1/p' "$ST" 2>/dev/null | tail -1)
  case "$ev" in prepared|detached|exited|error) break ;; esac
  kill -0 $HP 2>/dev/null || break
  sleep 0.5; i=$((i + 1))
done
last=$(tail -1 "$ST" 2>/dev/null || echo null)
case "$ev" in
  prepared|detached) ok=true ;;
  *) ok=false ;;
esac
echo "{\"ok\":$ok,\"event\":\"${ev:-none}\",\"helper_pid\":$HP,\"log\":\"$LOG\",\"last\":${last:-null}}"
[ "$ok" = true ]
