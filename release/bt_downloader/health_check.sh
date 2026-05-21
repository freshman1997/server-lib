#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
SERVER_BIN="${SERVER_BIN:-${BUILD_DIR}/release/bt_downloader/bt_downloader}"
BT_ADMIN_PORT="${BT_ADMIN_PORT:-18080}"

echo "[1/3] Checking binary"
[[ -x "${SERVER_BIN}" ]]

echo "[2/3] Checking server process"
if ! pgrep -af "bt_downloader" >/dev/null; then
  echo "bt_downloader is not running"
  exit 1
fi

echo "[3/3] Checking TCP listen port (${BT_ADMIN_PORT})"
ss -ltnp | grep ":${BT_ADMIN_PORT}" >/dev/null

echo "health check passed"
