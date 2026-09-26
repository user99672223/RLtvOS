#!/bin/sh
# 40-assets.sh — build the manifest and run the assets server in the
# background (nohup), logging to $OUT_DIR/assets-server.log. Idempotent:
# restarts the server if it is already running. Use `--stop` to stop it,
# `--rebuild` to regenerate the manifest, `--no-hash` to skip sha256.
set -eu
. "$(dirname "$0")/lib.sh"
need python3

PIDFILE="$OUT_DIR/assets-server.pid"
LOGFILE="$OUT_DIR/assets-server.log"
stop() {
  if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
    kill "$(cat "$PIDFILE")" && log "stopped assets server pid $(cat "$PIDFILE")"
  fi
  rm -f "$PIDFILE"
}
case "${1:-}" in --stop) stop; exit 0 ;; esac

[ -e "$ASSETS_DIR/game" ] || log "warning: $ASSETS_DIR/game missing (run 30-game.sh); serving without it"
stop
log "building manifest (first run hashes everything; later runs use the cache)"
python3 "$REPO_ROOT/laptop/assets_server.py" --manifest-only "$@" >&2
nohup python3 "$REPO_ROOT/laptop/assets_server.py" --port "$ASSETS_PORT" >>"$LOGFILE" 2>&1 &
echo $! > "$PIDFILE"
sleep 1
ip=${LAPTOP_IP:-$(hostname -I 2>/dev/null | awk '{print $1}')}
curl -fsS "http://127.0.0.1:$ASSETS_PORT/health" | head -c 600 >&2 || die "server did not come up; see $LOGFILE"
echo >&2
log "assets server pid $(cat "$PIDFILE") on http://$ip:$ASSETS_PORT/  (log: $LOGFILE)"
log "from the TV the app will fetch http://$ip:$ASSETS_PORT/manifest.jsonl.gz and /f/<root>/<path>"
