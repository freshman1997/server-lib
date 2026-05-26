#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
SERVER_BIN="${SERVER_BIN:-${BUILD_DIR}/release/nas/release_nas_server}"
NAS_PORT="${NAS_PORT:-8080}"

port_listening() {
  local port="$1"
  if command -v ss >/dev/null 2>&1; then
    ss -ltn | grep -E "[:.]${port}[[:space:]]" >/dev/null
    return $?
  fi
  if command -v netstat >/dev/null 2>&1; then
    netstat -an | grep -E "[:.]${port}[[:space:]].*LISTEN" >/dev/null
    return $?
  fi
  timeout 2 bash -c "cat < /dev/null > /dev/tcp/127.0.0.1/${port}" >/dev/null 2>&1
}

echo "[1/4] Checking binary"
[[ -x "${SERVER_BIN}" ]]

echo "[2/4] Checking server process"
if ! pgrep -af "release_nas_server" >/dev/null; then
  echo "release_nas_server is not running"
  exit 1
fi

echo "[3/4] Checking TCP listen port (${NAS_PORT})"
port_listening "${NAS_PORT}"

echo "[4/4] Checking health endpoint"
if command -v curl >/dev/null 2>&1; then
  curl -fsS "http://127.0.0.1:${NAS_PORT}/nas/health" | grep '"ok":true' >/dev/null
else
  echo "curl not found; TCP listen check passed"
fi

echo "health check passed"
