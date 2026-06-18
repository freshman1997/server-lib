#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
RUN_DIR="${RUN_DIR:-$BUILD_DIR/game_server_run}"

stop_service() {
  local name="$1"
  local pid_file="$RUN_DIR/$name.pid"
  if [[ ! -f "$pid_file" ]]; then
    return
  fi
  local pid
  pid="$(<"$pid_file")"
  if kill -0 "$pid" 2>/dev/null; then
    kill "$pid" 2>/dev/null || true
    for _ in {1..50}; do
      if ! kill -0 "$pid" 2>/dev/null; then
        break
      fi
      sleep 0.1
    done
    if kill -0 "$pid" 2>/dev/null; then
      kill -9 "$pid" 2>/dev/null || true
    fi
    echo "stopped $name pid=$pid"
  fi
  rm -f "$pid_file"
}

stop_service chat_web
stop_service rank
stop_service web
stop_service gateway
stop_service zone
stop_service world
stop_service global
stop_service world_db_proxy
stop_service player_db_proxy
stop_service tunnel

echo "all game services stopped"
