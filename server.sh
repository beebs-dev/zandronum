#!/bin/bash
set -euo pipefail

echo "Game ID: ${GAME_ID:-unset}"
echo "Using IWAD_ID: $IWAD_ID"
echo "Warp Level: ${WARP:-unset}"
echo "Using Game Skill: ${SKILL:-unset}"
echo "Using Data Root: ${DATA_ROOT:-unset}"

cd "$DATA_ROOT"

IWAD_PATH="$DATA_ROOT/${IWAD_ID:-}"
if [[ -z "${IWAD_ID:-}" ]]; then
  echo "ERROR: IWAD_ID env var is not set"
  exit 1
fi
if [[ ! -f "$IWAD_PATH" ]]; then
  echo "ERROR: IWAD_ID not found at $IWAD_PATH"
  exit 1
fi

ln -sf "$IWAD_PATH" "$IWAD_PATH.wad"

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
  -iwad "$IWAD_PATH.wad"
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
    ln -sf "$wad_path" "$wad_path.wad"
    SERVER+=( -file "$wad_path.wad" )
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
