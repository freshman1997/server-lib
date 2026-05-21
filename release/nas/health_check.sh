#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
SERVER_BIN="${SERVER_BIN:-${BUILD_DIR}/release/nas/release_nas_server}"
NAS_PORT="${NAS_PORT:-8080}"

echo "[1/3] Checking binary"
[[ -x "${SERVER_BIN}" ]]

echo "[2/3] Checking server process"
if ! pgrep -af "release_nas_server" >/dev/null; then
  echo "release_nas_server is not running"
  exit 1
fi

echo "[3/3] Checking TCP listen port (${NAS_PORT})"
ss -ltnp | grep ":${NAS_PORT}" >/dev/null

echo "health check passed"
