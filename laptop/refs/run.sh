#!/bin/sh
# run.sh — run checkpoint scripts natively in the rootfs under
#   strace -f -tt -y -s 160 -o refs/<name>.trace
# then gzip the trace, convert snapshots to PNG and write a syscall summary.
#
#   laptop/refs/run.sh [--display real] [--rw-rootfs] c1 [c2 ...] | all
#
# Outputs in $REFS_DIR: <name>.trace.gz, <name>.stdout, <name>.summary.txt,
# out/<name>/*.png|*.log. --display real binds the laptop's X display (for
# hardware Vulkan in e1/e2; the script then runs with RL_USE_REAL_DISPLAY=1).
set -eu
. "$(dirname "$0")/../setup/lib.sh"
need strace; need bwrap; need python3

opts=""; realdisp=0
while [ $# -gt 0 ]; do
  case "$1" in
    --display) opts="$opts --display $2"; [ "$2" = real ] && realdisp=1; shift 2 ;;
    --rw-rootfs) opts="$opts --rw-rootfs"; shift ;;
    *) break ;;
  esac
done
[ $# -gt 0 ] || die "usage: run.sh [--display real] [--rw-rootfs] name... | all"
names="$*"
[ "$names" = all ] && names="c1 c2 c3 c4 c5 d1 d2 d3 e1 e2"
mkdir -p "$REFS_DIR/out"

for n in $names; do
  [ -f "$REPO_ROOT/laptop/refs/guest/$n.sh" ] || die "no guest script laptop/refs/guest/$n.sh"
  log "=== $n ==="
  rm -f "$REFS_DIR/$n.trace" "$REFS_DIR/$n.trace.gz"
  mkdir -p "$REFS_DIR/out/$n"
  extra=""
  padpid=""
  if [ "$n" = d3 ]; then
    if [ -e /dev/uinput ]; then
      log "starting virtual gamepad (sudo, uinput) for 15s"
      sudo -b python3 "$REPO_ROOT/laptop/refs/host/virtual_pad.py" 15 >"$REFS_DIR/out/d3/virtual_pad.log" 2>&1 || true
      sleep 1
    else
      log "no /dev/uinput; d3 will read whatever /dev/input has"
    fi
    [ -d /dev/input ] && extra="--dev-bind /dev/input /dev/input"
  fi
  start=$(date +%s)
  rc=0
  # shellcheck disable=SC2086
  EXTRA_BWRAP="$extra" rootfs_run $opts --env RL_OUT=/refs/out/$n --env RL_USE_REAL_DISPLAY=$realdisp -- \
    strace -f -tt -y -s 160 -o "/refs/$n.trace" sh "/refs/guest/$n.sh" >"$REFS_DIR/$n.stdout" 2>&1 || rc=$?
  end=$(date +%s)
  log "$n: exit=$rc in $((end - start))s; stdout tail:"
  tail -5 "$REFS_DIR/$n.stdout" >&2 || true
  if [ -f "$REFS_DIR/$n.trace" ]; then
    gzip -f "$REFS_DIR/$n.trace"
    python3 "$REPO_ROOT/laptop/refs/summarize.py" "$REFS_DIR/$n.trace.gz" >"$REFS_DIR/$n.summary.txt" || true
    head -12 "$REFS_DIR/$n.summary.txt" >&2 || true
  else
    log "no trace produced for $n"
  fi
  # xwd → png (ImageMagick) for the result dir
  for x in "$REFS_DIR/out/$n"/*.xwd; do
    [ -f "$x" ] || continue
    if have convert; then convert "$x" "${x%.xwd}.png" 2>/dev/null && rm -f "$x"; fi
  done
  [ -n "$padpid" ] && kill "$padpid" 2>/dev/null || true
done
log "done. Files:"
ls -la "$REFS_DIR" | grep -E "trace.gz|summary|stdout" >&2 || true
