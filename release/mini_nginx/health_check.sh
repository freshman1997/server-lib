#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
SERVER_BIN="${SERVER_BIN:-${BUILD_DIR}/release/mini_nginx/mini_nginx}"
MINI_NGINX_PORT="${MINI_NGINX_PORT:-8080}"

echo "[1/3] Checking binary"
[[ -x "${SERVER_BIN}" ]]

echo "[2/3] Checking server process"
if ! pgrep -af "mini_nginx" >/dev/null; then
  echo "mini_nginx is not running"
  exit 1
fi

echo "[3/3] Checking TCP listen port (${MINI_NGINX_PORT})"
ss -ltnp | grep ":${MINI_NGINX_PORT}" >/dev/null

echo "health check passed"
