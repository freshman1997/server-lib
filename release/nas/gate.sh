#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
SERVER_BIN="${SERVER_BIN:-${BUILD_DIR}/release/nas/release_nas_server}"
CONFIG_FILE="${CONFIG_FILE:-${ROOT_DIR}/release/nas/config.json}"
PID_FILE="${PID_FILE:-${ROOT_DIR}/release/nas/release_nas_server.gate.pid}"
LOG_FILE="${LOG_FILE:-${ROOT_DIR}/release/nas/release_nas_server.gate.log}"
NAS_PORT="${NAS_PORT:-18080}"

cleanup() {
  if [[ -f "${PID_FILE}" ]]; then
    pid="$(cat "${PID_FILE}" || true)"
    if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
      kill "${pid}" 2>/dev/null || true
      sleep 0.2
      kill -KILL "${pid}" 2>/dev/null || true
    fi
    rm -f "${PID_FILE}"
  fi
}
trap cleanup EXIT

echo "[1/4] start release_nas_server"
"${SERVER_BIN}" --config "${CONFIG_FILE}" >"${LOG_FILE}" 2>&1 &
echo "$!" >"${PID_FILE}"
sleep 0.5

echo "[2/4] verify process alive"
pid="$(cat "${PID_FILE}")"
kill -0 "${pid}" 2>/dev/null

echo "[3/4] verify listen port (${NAS_PORT})"
ss -ltnp | grep ":${NAS_PORT}" >/dev/null

echo "[4/4] stop server"
cleanup

echo "nas gate passed"
