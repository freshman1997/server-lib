#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
SERVER_BIN="${SERVER_BIN:-${BUILD_DIR}/release/rtsp/release_rtsp_server}"

RTSP_PORT="${RTSP_PORT:-554}"

echo "[1/4] Checking binary"
[[ -x "${SERVER_BIN}" ]]

echo "[2/4] Checking server process"
if ! pgrep -af "release_rtsp_server" >/dev/null; then
  echo "release_rtsp_server is not running"
  exit 1
fi

echo "[3/4] Checking port listen (${RTSP_PORT})"
ss -ltnp | grep ":${RTSP_PORT}" >/dev/null

echo "[4/4] Checking RTSP/RTCP gate"
bash "${ROOT_DIR}/release/rtsp/scripts/run_rtsp_gate.sh" "${BUILD_DIR}" "rtsp|rtcp" >/dev/null

echo "health check passed"
