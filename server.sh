#!/bin/bash
set -euo pipefail

echo "Game ID: ${GAME_ID:-unset}"
echo "Using IWAD: $IWAD"
echo "Warp Level: ${WARP:-unset}"
echo "Using Game Skill: ${SKILL:-unset}"
echo "Using Data Root: ${DATA_ROOT:-unset}"

cd "$DATA_ROOT"

IWAD_PATH="$DATA_ROOT/${IWAD:-}"
if [[ -z "${IWAD:-}" ]]; then
  echo "ERROR: IWAD env var is not set"
  exit 1
fi
if [[ ! -f "$IWAD_PATH" ]]; then
  echo "ERROR: IWAD not found at $IWAD_PATH"
  exit 1
fi

SERVER=(
  /opt/zandronum/zandronum-server
  -iwad "$IWAD_PATH"
  +sv_coop_damagefactor 1.0
  +sv_defaultdmflags 0
  +sv_maxplayers 8
  +sv_pure false
)

if [[ -n "${WAD_LIST:-}" ]]; then
  IFS=',' read -r -a WADS <<< "$WAD_LIST"
  for wad in "${WADS[@]}"; do
    wad="${wad#"${wad%%[![:space:]]*}"}"  # ltrim
    wad="${wad%"${wad##*[![:space:]]}"}"  # rtrim
    [[ -z "$wad" ]] && continue

    wad_path="$DATA_ROOT/$wad"
    if [[ ! -f "$wad_path" ]]; then
      echo "ERROR: PWAD not found at $wad_path"
      exit 1
    fi
    SERVER+=( -file "$wad_path" )
  done
fi

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
