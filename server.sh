#!/bin/bash
set -euo pipefail

echo "Game ID: ${GAME_ID:-unset}"
echo "Using IWAD_ID: $IWAD_ID"
echo "Warp Level: ${WARP:-unset}"
echo "Using Game Skill: ${SKILL:-unset}"
echo "Using Data Root: ${DATA_ROOT:-unset}"

cd "$DATA_ROOT"

resolve_by_id() {
  local id="$1"

  # 1) Old behavior: file named exactly the ID
  if [[ -f "$id" ]]; then
    printf '%s\n' "$id"
    return 0
  fi

  # 2) New behavior: ID.<ext> (preserve extension)
  local matches=()
  # Null-delimited to be safe with odd filenames; IDs won't have spaces, but extensions might.
  while IFS= read -r -d '' f; do
    matches+=("$f")
  done < <(find . -maxdepth 1 -type f -name "${id}.*" -print0)

  if (( ${#matches[@]} == 0 )); then
    echo "ERROR: Could not resolve file for id=$id in $DATA_ROOT (looked for '$id' or '${id}.*')" >&2
    ls -al "$DATA_ROOT" >&2 || true
    return 1
  fi
  if (( ${#matches[@]} > 1 )); then
    echo "ERROR: Ambiguous files for id=$id (more than one match):" >&2
    printf '  %s\n' "${matches[@]}" >&2
    return 1
  fi

  # strip leading ./ from find output
  printf '%s\n' "${matches[0]#./}"
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
  -iwad "iwad.wad"
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
