#!/bin/bash
set -euo pipefail
SERVER_ADDR=${SERVER_ADDR:-localhost:10666}
IWAD_PATH=${IWAD_PATH:-/var/wads/iwad.wad}
WAD_LIST=${WAD_LIST:-""}
STARTUP_DELAY_SECONDS=${STARTUP_DELAY_SECONDS:-10}
GAME_ID=${GAME_ID:-unset}
MASTER_BASE_URL=${MASTER_BASE_URL:-http://dorch-master}

RESOLVE_SCRIPT_PATH=$(dirname "$0")/resolve-by-id.sh
resolve_by_id() {
  "$RESOLVE_SCRIPT_PATH" $DATA_ROOT "$1"
}

cd ${DATA_ROOT:-/var/wads}

echo "Spectator is sleeping for $STARTUP_DELAY_SECONDS seconds before starting."
sleep "$STARTUP_DELAY_SECONDS"
while [[ ! -f "$IWAD_PATH" ]]; do
  echo "Waiting for IWAD at $IWAD_PATH ..."
  sleep 5
done
echo "Spectator starting, connecting to $SERVER_ADDR using IWAD $IWAD_PATH"

CLIENT=(
  /opt/zandronum/zandronum
  -iwad "$IWAD_PATH"
  -connect "$SERVER_ADDR"
  -width 640 -height 480
  -nosound -nomusic
  -soft
  -fullscreen
)
if [[ -n "${WAD_LIST:-}" ]]; then
  IFS=',' read -r -a WADS <<< "$WAD_LIST"
  for wad in "${WADS[@]}"; do
    wad="${wad#"${wad%%[![:space:]]*}"}"  # ltrim
    wad="${wad%"${wad##*[![:space:]]}"}"  # rtrim
    [[ -z "$wad" ]] && continue
    wad_path="$(resolve_by_id "$wad")"
    echo "Resolved PWAD id '$wad' to path '$wad_path'"
    if [[ ! -f "$wad_path" ]]; then
      echo "ERROR: PWAD not found after resolve: $wad_path"
      echo "Data root: $DATA_ROOT"
      ls -al $DATA_ROOT >&2 || true
      echo "WAD dir: $(dirname "$wad_path")"
      ls -al "$(dirname "$wad_path")" >&2 || true
      exit 1
    fi
    echo "Adding PWAD: $wad_path"
    CLIENT+=( -file "$wad_path" )
  done
fi

rm -f /screenshot.png /screenshot.jpg /screenshot.jpg.tmp

# Run the client in a virtual X server.
# NOTE: Zandronum still needs an X display for rendering, even for screenshots.
echo ">>> ${CLIENT[*]}"
xvfb-run -a -s "-screen 0 640x480x24" "${CLIENT[@]}" &

game_pid=$!

term() {
  echo "Got SIGTERM, forwarding to client pid=$game_pid"
  kill -TERM "$game_pid" 2>/dev/null || true
}
trap term TERM INT

pick_latest_png() {
  # Zandronum/GZDoom family often writes screenshots to a "screenshots" directory
  # under either the working dir or the user's config dir.
  local -a candidates=()
  local p
  for p in \
    "/screenshot.png" \
    "$PWD/screenshot.png" \
    "/Screenshot_"*.png \
    "/screenshots"/*.png \
    "$PWD/screenshots"/*.png \
    "$HOME/.zandronum/screenshots"/*.png \
    "$HOME/.config/zandronum/screenshots"/*.png \
    "$HOME/.config/zandronum"/*.png \
  ; do
    [[ -f "$p" ]] && candidates+=("$p")
  done

  [[ ${#candidates[@]} -eq 0 ]] && return 1

  local newest=""
  local newest_mtime=0
  local mtime
  for p in "${candidates[@]}"; do
    mtime=$(stat -c %Y "$p" 2>/dev/null || echo "")
    [[ -z "$mtime" ]] && continue
    if (( mtime > newest_mtime )); then
      newest_mtime=$mtime
      newest="$p"
    fi
  done

  [[ -z "$newest" ]] && return 1
  printf '%s\n' "$newest"
}

echo "Screenshot watcher started (PID=$$). Expecting /screenshot.png and /screenshot.jpg outputs."

last_src=""
last_src_mtime=""
loop_i=0
while kill -0 "$game_pid" 2>/dev/null; do
  loop_i=$((loop_i + 1))

  src_png=""
  if src_png=$(pick_latest_png 2>/dev/null); then
    src_mtime=$(stat -c %Y "$src_png" 2>/dev/null || echo "")
    if [[ -n "$src_mtime" && ( "$src_png" != "$last_src" || "$src_mtime" != "$last_src_mtime" ) ]]; then
      echo "[watcher] New screenshot source detected: $src_png (mtime=$src_mtime)"
      last_src="$src_png"
      last_src_mtime="$src_mtime"

      # Always copy to the expected location, then generate the JPG.
      if [[ "$src_png" != "/screenshot.png" ]]; then
        echo "[watcher] Copying $src_png -> /screenshot.png"
        cp -f "$src_png" /screenshot.png
      else
        echo "[watcher] Using /screenshot.png directly"
      fi

      echo "[watcher] Converting /screenshot.png -> /screenshot.jpg (resize 640x480!)"
      convert /screenshot.png -resize 640x480\! /screenshot.jpg.tmp
      mv -f /screenshot.jpg.tmp /screenshot.jpg

      out_sz=$(stat -c %s /screenshot.jpg 2>/dev/null || echo "")
      echo "[watcher] Wrote /screenshot.jpg (bytes=${out_sz:-?})"
    fi
  else
    # Emit a heartbeat occasionally so container logs show we're alive.
    if (( loop_i % 10 == 0 )); then
      echo "[watcher] No screenshot found yet. Looked in: /, $PWD, $PWD/screenshots, $HOME/.zandronum/screenshots, $HOME/.config/zandronum/screenshots"
    fi
  fi

  sleep 1
done

wait "$game_pid"
