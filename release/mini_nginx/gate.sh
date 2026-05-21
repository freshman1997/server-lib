#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
SERVER_BIN="${SERVER_BIN:-${BUILD_DIR}/release/mini_nginx/mini_nginx}"
CONFIG_FILE="${CONFIG_FILE:-${ROOT_DIR}/release/mini_nginx/mini_nginx.json}"
PID_FILE="${PID_FILE:-${ROOT_DIR}/release/mini_nginx/mini_nginx.gate.pid}"
LOG_FILE="${LOG_FILE:-${ROOT_DIR}/release/mini_nginx/mini_nginx.gate.log}"
MINI_NGINX_PORT="${MINI_NGINX_PORT:-18080}"

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

echo "[1/4] start mini_nginx"
YUAN_MINI_NGINX_PORT="${MINI_NGINX_PORT}" "${SERVER_BIN}" "${CONFIG_FILE}" >"${LOG_FILE}" 2>&1 &
echo "$!" >"${PID_FILE}"
sleep 0.5

echo "[2/4] verify process alive"
pid="$(cat "${PID_FILE}")"
kill -0 "${pid}" 2>/dev/null

echo "[3/4] verify listen port (${MINI_NGINX_PORT})"
ss -ltnp | grep ":${MINI_NGINX_PORT}" >/dev/null

echo "[4/4] stop server"
cleanup

echo "mini_nginx gate passed"
