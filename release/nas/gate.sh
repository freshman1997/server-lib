#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
SERVER_BIN="${SERVER_BIN:-${BUILD_DIR}/release/nas/release_nas_server}"
CONFIG_FILE="${CONFIG_FILE:-${ROOT_DIR}/release/nas/config.json}"
PID_FILE="${PID_FILE:-${ROOT_DIR}/release/nas/release_nas_server.gate.pid}"
LOG_FILE="${LOG_FILE:-${ROOT_DIR}/release/nas/release_nas_server.gate.log}"
NAS_PORT="${NAS_PORT:-8080}"
REDIS_HOST="${REDIS_HOST:-127.0.0.1}"
REDIS_PORT="${REDIS_PORT:-6379}"
NAS_ADMIN_USER="${YUAN_NAS_ADMIN_USER:-}"
NAS_ADMIN_PASSWORD="${YUAN_NAS_ADMIN_PASSWORD:-}"

tcp_available() {
  local host="$1"
  local port="$2"
  timeout 2 bash -c "cat < /dev/null > /dev/tcp/${host}/${port}" >/dev/null 2>&1
}

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
  tcp_available "127.0.0.1" "${port}"
}

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

echo "[1/6] verify Redis (${REDIS_HOST}:${REDIS_PORT})"
if ! tcp_available "${REDIS_HOST}" "${REDIS_PORT}"; then
  echo "SKIP: Redis is not reachable; production NAS gate requires metadata storage"
  exit 77
fi

echo "[2/6] start release_nas_server"
"${SERVER_BIN}" --config "${CONFIG_FILE}" >"${LOG_FILE}" 2>&1 &
echo "$!" >"${PID_FILE}"
sleep 0.5

echo "[3/6] verify process alive"
pid="$(cat "${PID_FILE}")"
kill -0 "${pid}" 2>/dev/null

echo "[4/6] verify listen port (${NAS_PORT})"
port_listening "${NAS_PORT}"

echo "[5/6] verify health endpoint"
if command -v curl >/dev/null 2>&1; then
  curl -fsS "http://127.0.0.1:${NAS_PORT}/nas/health" | grep '"ok":true' >/dev/null
else
  echo "curl not found; TCP listen check passed"
fi

echo "[6/6] verify admin readiness"
if [[ -n "${NAS_ADMIN_USER}" && -n "${NAS_ADMIN_PASSWORD}" ]]; then
  if ! command -v curl >/dev/null 2>&1; then
    echo "curl is required for authenticated readiness check" >&2
    exit 1
  fi
  curl -fsS -u "${NAS_ADMIN_USER}:${NAS_ADMIN_PASSWORD}" \
    "http://127.0.0.1:${NAS_PORT}/nas/admin/readiness" | grep '"ready":true' >/dev/null
elif [[ "${YUAN_NAS_GATE_REQUIRE_READINESS:-}" == "1" ]]; then
  echo "YUAN_NAS_ADMIN_USER/YUAN_NAS_ADMIN_PASSWORD are required when YUAN_NAS_GATE_REQUIRE_READINESS=1" >&2
  exit 1
else
  echo "SKIP: set YUAN_NAS_ADMIN_USER and YUAN_NAS_ADMIN_PASSWORD to verify /nas/admin/readiness"
fi

cleanup

echo "nas gate passed"
