#!/bin/bash
set -euo pipefail
RESOLVE_SCRIPT_PATH=$(dirname "$0")/resolve-by-id.sh
echo "Game ID: ${GAME_ID:-unset}"
echo "Server Name: ${SERVER_NAME:-Zandronum Server}"
echo "Using IWAD_ID: $IWAD_ID"
echo "Warp Level: ${WARP:-unset}"
echo "Using Game Skill: ${SKILL:-unset}"
echo "Using Data Root: ${DATA_ROOT:-unset}"

MAX_PLAYERS=${MAX_PLAYERS:-32}
echo "Using Max Players: $MAX_PLAYERS"

cd "$DATA_ROOT"

resolve_by_id() {
  "$RESOLVE_SCRIPT_PATH" $DATA_ROOT "$1"
}

if [[ -z "${IWAD_ID:-}" ]]; then
  echo "ERROR: IWAD_ID env var is not set"
  exit 1
fi

IWAD_PATH="$(resolve_by_id "$IWAD_ID")"
if [[ ! -f "$IWAD_PATH" ]]; then
  echo "ERROR: IWAD not found after resolve: $IWAD_PATH"
  exit 1
fi

# Stable name for zandronum
ln -sf "$IWAD_PATH" "iwad.wad"

magic=$(head -c 4 "$IWAD_PATH" | xxd -p)
if [[ "$magic" != "49574144" ]]; then  # "IWAD"
  echo "ERROR: $IWAD_PATH does not look like an IWAD (magic=$magic)"
  file "$IWAD_PATH" || true
  exit 1
else
  echo "IWAD verified at $IWAD_PATH"
fi

echo "IWAD SHA256: $(sha256sum "$IWAD_PATH" || true)"

SERVER=(
  /opt/zandronum/zandronum-server
  -config /server.ini
  -iwad "iwad.wad"
  -skill ${SKILL:-3}
  +map ${WARP:-"MAP01"}
  +sv_coop_damagefactor 1.0
  +sv_defaultdmflags 0
  +sv_maxclientsperip 0
  +sv_maxplayers $MAX_PLAYERS
  +sv_doubleammo 1
  +sv_weaponstay 1
  +sv_itemrespawn 1
  +sv_hostname "$SERVER_NAME"
)

if [[ -n "${WAD_LIST:-}" ]]; then
  IFS=',' read -r -a WADS <<< "$WAD_LIST"
  for wad in "${WADS[@]}"; do
    wad="${wad#"${wad%%[![:space:]]*}"}"  # ltrim
    wad="${wad%"${wad##*[![:space:]]}"}"  # rtrim
    [[ -z "$wad" ]] && continue
    wad_path="$(resolve_by_id "$wad")"
    if [[ ! -f "$wad_path" ]]; then
      echo "ERROR: PWAD not found after resolve: $wad_path"
      exit 1
    fi
    echo "Adding PWAD: $wad_path"
    SERVER+=( -file "$wad_path" )
  done
fi

echo ">>> ${SERVER[*]}"
"${SERVER[@]}" &
pid=$!

term() {
  echo "Got SIGTERM, forwarding to server pid=$pid"
  kill -TERM "$pid" 2>/dev/null || true
}

trap term TERM INT
wait "$pid"
