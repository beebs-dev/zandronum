#!/bin/bash
set -u

cd "$(dirname "$0")"

child_pid=""
stopping=0
child_group_leader=0

forward_term() {
  stopping=1
  echo "[$(date -Is)] received TERM/INT; stopping..."
  if [[ -n "${child_pid}" ]] && kill -0 "$child_pid" 2>/dev/null; then
    # If the child is a process-group leader, signaling -$child_pid will
    # terminate the whole subtree. Fall back to killing just the child PID.
    if (( child_group_leader )); then
      kill -TERM -- "-${child_pid}" 2>/dev/null || true
      kill -KILL -- "-${child_pid}" 2>/dev/null || true
    else
      kill -TERM "$child_pid" 2>/dev/null || true
      kill -KILL "$child_pid" 2>/dev/null || true
    fi
  fi

  # Exit immediately. Do not wait for clean shutdown.
  exit 0
}

trap forward_term TERM INT

while true; do
  echo "[$(date -Is)] starting spectator process..."
  # Prefer running the child in its own session/process group so we can kill
  # the whole process tree quickly on SIGTERM.
  if command -v setsid >/dev/null 2>&1; then
    setsid ./spectator-inner.sh &
    child_group_leader=1
  else
    ./spectator-inner.sh &
    child_group_leader=0
  fi
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
