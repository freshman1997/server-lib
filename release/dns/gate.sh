#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
SERVER_BIN="${SERVER_BIN:-${BUILD_DIR}/release/dns/release_dns_server}"
CONFIG_FILE="${CONFIG_FILE:-${ROOT_DIR}/release/dns/config.json}"
PID_FILE="${PID_FILE:-${ROOT_DIR}/release/dns/release_dns_server.gate.pid}"
LOG_FILE="${LOG_FILE:-${ROOT_DIR}/release/dns/release_dns_server.gate.log}"
DNS_PORT="${DNS_PORT:-5353}"

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

echo "[1/4] start release_dns_server"
"${SERVER_BIN}" --config "${CONFIG_FILE}" --port "${DNS_PORT}" >"${LOG_FILE}" 2>&1 &
echo "$!" >"${PID_FILE}"
sleep 0.3

echo "[2/4] run in-process self-check"
"${SERVER_BIN}" --port "${DNS_PORT}" --self-check-only >/dev/null

echo "[3/4] verify process still alive"
pid="$(cat "${PID_FILE}")"
kill -0 "${pid}" 2>/dev/null

echo "[4/4] stop server"
cleanup

echo "dns gate passed"
