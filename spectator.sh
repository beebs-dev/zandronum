#!/bin/bash
cd "$(dirname "$0")"
set -u
while true; do
  echo "[$(date -Is)] starting spectator.sh"
  ./spectator-inner.sh
  exit_code=$?
  echo "[$(date -Is)] spectator.sh exited with code $exit_code, restarting..."
  sleep 1
done
