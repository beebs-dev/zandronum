#!/bin/bash
set -euo pipefail
echo "Game ID: ${GAME_ID:-unset}"
echo "Using IWAD: $IWAD"
echo "Warp Level: ${WARP:-unset}"
echo "Using Game Skill: ${SKILL:-unset}"
echo "Using Data Root: ${DATA_ROOT:-unset}"
PORT="${GAME_PORT:-10666}"
CMD=(
    /opt/zandronum/zandronum-server
)
# Zandronum will look for wads in the cwd
cd "$DATA_ROOT"
echo ">>> ${CMD[*]}"
exec "${CMD[@]}"
