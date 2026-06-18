#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
BIN_DIR="$BUILD_DIR/bin"
CONFIG_DIR="$BUILD_DIR/libs/game/server/config"
RUN_DIR="${RUN_DIR:-$BUILD_DIR/game_server_run}"
LOG_DIR="$RUN_DIR/logs"

mkdir -p "$LOG_DIR"

start_service() {
  local name="$1"
  local bin="$2"
  local config="$3"
  if [[ ! -x "$BIN_DIR/$bin" ]]; then
    echo "missing executable: $BIN_DIR/$bin" >&2
    exit 1
  fi
  if [[ ! -f "$CONFIG_DIR/$config" ]]; then
    echo "missing config: $CONFIG_DIR/$config" >&2
    exit 1
  fi
  if [[ -f "$RUN_DIR/$name.pid" ]] && kill -0 "$(<"$RUN_DIR/$name.pid")" 2>/dev/null; then
    echo "$name already running pid=$(<"$RUN_DIR/$name.pid")"
    return
  fi
  "$BIN_DIR/$bin" "$CONFIG_DIR/$config" >"$LOG_DIR/$name.log" 2>&1 &
  echo $! >"$RUN_DIR/$name.pid"
  echo "started $name pid=$(<"$RUN_DIR/$name.pid") log=$LOG_DIR/$name.log"
}

start_service tunnel game_tunnel_server tunnel.json
sleep 0.2
start_service player_db_proxy game_player_db_proxy_server player_db_proxy.json
start_service world_db_proxy game_world_db_proxy_server world_db_proxy.json
start_service global game_global_server global.json
start_service world game_world_server world.json
sleep 0.2
start_service zone game_zone_server zone.json
sleep 0.2
start_service gateway game_gateway_server gateway.json
start_service web game_web_server web.json
start_service rank game_rank_server rank.json
start_service chat_web game_chat_web_server chat_web.json

echo "all game services started; pid files in $RUN_DIR"
