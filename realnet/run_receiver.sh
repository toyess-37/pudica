#!/usr/bin/env bash
# Visible: run_receiver.sh PORT --visible
# Automated decoding: run_receiver.sh PORT --decode-only
# Synthetic transport: run_receiver.sh PORT --headless
set -euo pipefail
PORT="${1:?usage: run_receiver.sh PORT [--visible|--decode-only|--headless]}"
MODE="${2:---visible}"
[[ "$PORT" =~ ^[0-9]+$ ]] && ((PORT>=1 && PORT<=65535)) || { echo "invalid port" >&2; exit 2; }
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$HERE/codes/receiver"
[[ -x "$BIN" ]] || { echo "build with make -C codes" >&2; exit 1; }
if [[ "$MODE" == --decode-only || "$MODE" == --headless ]]; then
  exec "$BIN" "$PORT" "$MODE"
fi
[[ "$MODE" == --visible ]] || { echo "unknown mode: $MODE" >&2; exit 2; }
DISPLAY_NUM="${DISPLAY:-${XVFB_DISPLAY:-:99}}"
GEOM="${XVFB_GEOMETRY:-1280x720x24}"
xvfb_pid=""
receiver_pid=""
cleanup() {
  trap - EXIT INT TERM
  for pid in "$receiver_pid" "$xvfb_pid"; do
    if [[ -n "$pid" ]]; then
      kill -TERM "$pid" 2>/dev/null || true
      for _ in {1..30}; do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
      kill -KILL "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
  done
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
if ! DISPLAY="$DISPLAY_NUM" xdpyinfo >/dev/null 2>&1; then
  Xvfb "$DISPLAY_NUM" -screen 0 "$GEOM" -nolisten tcp >/dev/null 2>&1 &
  xvfb_pid=$!
  ready=0
  for _ in {1..50}; do
    kill -0 "$xvfb_pid" 2>/dev/null || break
    if DISPLAY="$DISPLAY_NUM" xdpyinfo >/dev/null 2>&1; then ready=1; break; fi
    sleep 0.1
  done
  ((ready==1)) || { echo "Xvfb readiness failed" >&2; exit 1; }
fi
env DISPLAY="$DISPLAY_NUM" SDL_VIDEODRIVER=x11 "$BIN" "$PORT" &
receiver_pid=$!
wait "$receiver_pid"
receiver_pid=""
