#!/bin/bash
set -euo pipefail

echo "Game ID: ${GAME_ID:-unset}"
echo "Using IWAD: $IWAD"
echo "Warp Level: ${WARP:-unset}"
echo "Using Game Skill: ${SKILL:-unset}"
echo "Using Data Root: ${DATA_ROOT:-unset}"

cd "$DATA_ROOT"

SERVER=(/opt/zandronum/zandronum-server)

echo ">>> ${SERVER[*]}"
"${SERVER[@]}" &
pid=$!

term() {
  echo "Got SIGTERM, forwarding to server pid=$pid"
  kill -TERM "$pid" 2>/dev/null || true

  # If TERM doesn’t work for zandronum, try INT instead:
  # kill -INT "$pid" 2>/dev/null || true
}

trap term TERM INT
wait "$pid"
