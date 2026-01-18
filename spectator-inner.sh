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
  +cl_invisiblespectator 1
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

rm -f /screenshot.png /screenshot.png.tmp /screenshot.webp /screenshot.webp.tmp

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

# Upload logic: POST /screenshot.webp to the master and delete it on success.
# On failure, keep the image and back off with jitter to avoid churn.
upload_failures=0
upload_next_epoch=0

calc_backoff_delay_seconds() {
  local failures="$1"

  local base=3
  local max_cap=$((14 + RANDOM % 3)) # 14-16 seconds
  local delay=$base

  # Exponential backoff: base * 2^failures, capped to max_cap.
  local i
  for (( i=0; i<failures; i++ )); do
    delay=$((delay * 2))
    (( delay >= max_cap )) && break
  done
  (( delay > max_cap )) && delay=$max_cap

  # Jitter 0-2 seconds, keep within cap.
  local jitter=$((RANDOM % 3))
  delay=$((delay + jitter))
  (( delay > max_cap )) && delay=$max_cap

  echo "$delay"
}

try_upload_liveshot() {
  [[ -f /screenshot.webp ]] || return 0

  if [[ "${GAME_ID:-unset}" == "unset" ]]; then
    echo "[upload] GAME_ID is unset; skipping upload"
    return 0
  fi

  local now
  now=$(date +%s)
  if (( now < upload_next_epoch )); then
    return 0
  fi

  local url="${MASTER_BASE_URL%/}/game/${GAME_ID}/liveshot"
  echo "[upload] POST $url (bytes=$(stat -c %s /screenshot.webp 2>/dev/null || echo "?"))"

  if curl -fsS \
      --connect-timeout 2 \
      --max-time 10 \
      -X POST \
      -H "Content-Type: image/webp" \
      --data-binary @/screenshot.webp \
      "$url" \
      >/dev/null; then
    echo "[upload] OK; deleting /screenshot.webp"
    rm -f /screenshot.webp
    upload_failures=0
    upload_next_epoch=0
    return 0
  fi

  upload_failures=$((upload_failures + 1))
  local delay
  delay=$(calc_backoff_delay_seconds "$upload_failures")
  now=$(date +%s)
  upload_next_epoch=$((now + delay))
  echo "[upload] FAILED; keeping /screenshot.webp, backing off ${delay}s (failures=$upload_failures)"
  sleep "$delay"
  return 1
}

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

echo "Screenshot watcher started (PID=$$). Expecting /screenshot.png and /screenshot.webp outputs."

last_src=""
last_src_mtime=""
loop_i=0
while kill -0 "$game_pid" 2>/dev/null; do
  loop_i=$((loop_i + 1))

  # If a previous upload failed, keep retrying it with backoff.
  try_upload_liveshot || true

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

      if [[ -f /screenshot.webp ]]; then
        echo "[watcher] /screenshot.webp pending upload; not overwriting"
      else
        echo "[watcher] Converting /screenshot.png -> /screenshot.webp (resize 640x480!, quality=70)"
        convert /screenshot.png -resize 640x480\! -quality 70 /screenshot.webp.tmp
        mv -f /screenshot.webp.tmp /screenshot.webp

        out_sz=$(stat -c %s /screenshot.webp 2>/dev/null || echo "")
        echo "[watcher] Wrote /screenshot.webp (bytes=${out_sz:-?})"
      fi

      # Attempt upload immediately after producing a new shot.
      try_upload_liveshot || true
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
