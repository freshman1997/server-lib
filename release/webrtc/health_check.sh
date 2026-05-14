#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
SERVER_BIN="${SERVER_BIN:-${BUILD_DIR}/release/webrtc/release_webrtc_server}"
WEBRTC_PORT="${WEBRTC_PORT:-9000}"

echo "[1/4] Checking binary"
[[ -x "${SERVER_BIN}" ]]

echo "[2/4] Checking server process"
if ! pgrep -af "release_webrtc_server" >/dev/null; then
  echo "release_webrtc_server is not running"
  exit 1
fi

echo "[3/4] Checking port listen (${WEBRTC_PORT})"
ss -ltnp | grep ":${WEBRTC_PORT}" >/dev/null

echo "[4/4] Checking self-check endpoint"
"${SERVER_BIN}" --self-check-only --probe-host 127.0.0.1 --port "${WEBRTC_PORT}" >/dev/null

echo "health check passed"
