#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
SERVER_BIN="${SERVER_BIN:-${BUILD_DIR}/release/bt_downloader/bt_downloader}"
CONFIG_FILE="${CONFIG_FILE:-${ROOT_DIR}/release/bt_downloader/config.json}"
PID_FILE="${PID_FILE:-${ROOT_DIR}/release/bt_downloader/bt_downloader.gate.pid}"
LOG_FILE="${LOG_FILE:-${ROOT_DIR}/release/bt_downloader/bt_downloader.gate.log}"
BT_ADMIN_PORT="${BT_ADMIN_PORT:-18080}"

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

echo "[1/4] start bt_downloader"
YUAN_BT_ADMIN_PORT="${BT_ADMIN_PORT}" "${SERVER_BIN}" "${CONFIG_FILE}" >"${LOG_FILE}" 2>&1 &
echo "$!" >"${PID_FILE}"
sleep 0.5

echo "[2/4] verify process alive"
pid="$(cat "${PID_FILE}")"
kill -0 "${pid}" 2>/dev/null

echo "[3/4] verify listen port (${BT_ADMIN_PORT})"
ss -ltnp | grep ":${BT_ADMIN_PORT}" >/dev/null

echo "[4/4] stop server"
cleanup

echo "bt_downloader gate passed"
