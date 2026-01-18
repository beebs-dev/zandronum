#!/bin/bash
set -euo pipefail

# Exit immediately on SIGTERM/SIGINT. Do not wait for child processes and do
# not attempt to upload/flush any work.
fast_exit() {
  set +e
  trap - TERM INT
  echo "Got SIGTERM/INT; exiting immediately"

  # Kill any background jobs we started (game/Xvfb/ffmpeg).
  local pids
  pids=$(jobs -pr 2>/dev/null || true)
  if [[ -n "${pids}" ]]; then
    kill -KILL ${pids} 2>/dev/null || true
  fi

  exit 0
}
trap fast_exit TERM INT

SERVER_ADDR=${SERVER_ADDR:-localhost:10666}
IWAD_PATH=${IWAD_PATH:-/var/wads/iwad.wad}
WAD_LIST=${WAD_LIST:-""}
STARTUP_DELAY_SECONDS=${STARTUP_DELAY_SECONDS:-10}
GAME_ID=${GAME_ID:-unset}
MASTER_BASE_URL=${MASTER_BASE_URL:-http://dorch-master}
RTMP_ENDPOINT=${RTMP_ENDPOINT:-""}

resolve_by_id() {
  /resolve-by-id.sh $DATA_ROOT "$1"
}

cd ${DATA_ROOT:-/var/wads}

echo "Spectator is sleeping for $STARTUP_DELAY_SECONDS seconds before starting."
sleep "$STARTUP_DELAY_SECONDS"
while [[ ! -f "$IWAD_PATH" ]]; do
  echo "Waiting for IWAD at $IWAD_PATH ..."
  sleep 5
done
echo "Spectator starting, connecting to $SERVER_ADDR using IWAD $IWAD_PATH"

# Start a local PulseAudio daemon so we can capture game audio from a monitor
# source (stream.monitor) and push it out via ffmpeg.
have_pulse=0
if command -v pulseaudio >/dev/null 2>&1 && command -v pactl >/dev/null 2>&1; then
  export SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-pulse}

  # Try system-mode first (works well in containers), then fall back.
  pulseaudio --system --daemonize=yes --exit-idle-time=-1 --disallow-exit --log-target=stderr 2>/dev/null || \
  pulseaudio --daemonize=yes --exit-idle-time=-1 --disallow-exit --log-target=stderr 2>/dev/null || true

  # Give PulseAudio a moment to create its socket.
  sleep 0.2

  if pactl info >/dev/null 2>&1; then
    pactl load-module module-null-sink sink_name=stream sink_properties=device.description=stream >/dev/null 2>&1 || true
    pactl set-default-sink stream >/dev/null 2>&1 || true

    if pactl list short sources 2>/dev/null | awk '{print $2}' | grep -qx 'stream.monitor'; then
      have_pulse=1
    fi
  fi
fi

if [[ "$have_pulse" == "1" ]]; then
  echo "PulseAudio ready; audio will be captured from stream.monitor"
else
  echo "PulseAudio unavailable; RTMP audio will be silent"
fi

if [[ "${DEBUG_AUDIO:-0}" == "1" ]]; then
  echo "[audio] SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-}"
  if command -v pactl >/dev/null 2>&1; then
    echo "[audio] pactl info:" && pactl info || true
    echo "[audio] sinks:" && pactl list short sinks || true
    echo "[audio] sources:" && pactl list short sources || true
    echo "[audio] default sink:" && pactl get-default-sink || true
    echo "[audio] default source:" && pactl get-default-source || true
  fi
fi

CLIENT=(
  /opt/zandronum/zandronum
  -iwad "$IWAD_PATH"
  -connect "$SERVER_ADDR"
  -width 640 -height 480
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
DISPLAY_NUMBER=${DISPLAY_NUMBER:-99}
export DISPLAY=":${DISPLAY_NUMBER}"

Xvfb "$DISPLAY" -screen 0 640x480x24 -nolisten tcp -ac &
xvfb_pid=$!
sleep 0.2

echo ">>> ${CLIENT[*]}"
"${CLIENT[@]}" &
game_pid=$!

ffmpeg_pid=""
if [[ -n "${RTMP_ENDPOINT}" ]]; then
  echo "Starting RTMP stream to endpoint (redacted)."

  audio_in_args=( -f lavfi -i "anullsrc=channel_layout=stereo:sample_rate=44100" )
  if [[ "$have_pulse" == "1" ]]; then
    audio_in_args=( -f pulse -i stream.monitor )
  fi

  ffmpeg -hide_banner -loglevel warning \
    -f x11grab -video_size 640x480 -framerate 30 -i "${DISPLAY}.0" \
    "${audio_in_args[@]}" \
    -c:v libx264 -preset veryfast -tune zerolatency -pix_fmt yuv420p -g 60 -keyint_min 60 \
    -c:a aac -b:a 128k -ar 44100 \
    -f flv "${RTMP_ENDPOINT}" &
  ffmpeg_pid=$!
fi

term() { fast_exit; }
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

if [[ -n "${ffmpeg_pid}" ]] && kill -0 "$ffmpeg_pid" 2>/dev/null; then
  kill -TERM "$ffmpeg_pid" 2>/dev/null || true
fi
if [[ -n "${xvfb_pid:-}" ]] && kill -0 "$xvfb_pid" 2>/dev/null; then
  kill -TERM "$xvfb_pid" 2>/dev/null || true
fi
