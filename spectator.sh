#!/bin/bash
set -u

cd "$(dirname "$0")"

child_pid=""
stopping=0

forward_term() {
  stopping=1
  echo "[$(date -Is)] received TERM/INT; stopping..."
  if [[ -n "${child_pid}" ]] && kill -0 "$child_pid" 2>/dev/null; then
    kill -TERM "$child_pid" 2>/dev/null || true
  fi
}

trap forward_term TERM INT

while true; do
  echo "[$(date -Is)] starting spectator process..."
  ./spectator-inner.sh &
  child_pid=$!

  # Wait for child to exit (or be killed)
  wait "$child_pid"
  exit_code=$?
  child_pid=""

  if (( stopping )); then
    echo "[$(date -Is)] child exited ($exit_code) during shutdown; exiting wrapper."
    exit "$exit_code"
  fi

  echo "[$(date -Is)] spectator process exited with code $exit_code, restarting..."
  sleep 1
done
