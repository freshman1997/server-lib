#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${YUAN_PROXY_SERVER_BIN:-${SCRIPT_DIR}/../../build/release/proxy/release_proxy_server}"
CONFIG="${YUAN_PROXY_CONFIG:-${SCRIPT_DIR}/config.json}"
PID_FILE="${YUAN_PROXY_PID_FILE:-${SCRIPT_DIR}/proxy.pid}"
LOG_FILE="${YUAN_PROXY_LOG_FILE:-${SCRIPT_DIR}/proxy.log}"

if [[ -f "${PID_FILE}" ]] && kill -0 "$(cat "${PID_FILE}")" 2>/dev/null; then
  echo "proxy already running pid=$(cat "${PID_FILE}")"
  exit 0
fi

mkdir -p "$(dirname "${LOG_FILE}")"
nohup "${BIN}" "${CONFIG}" >>"${LOG_FILE}" 2>&1 &
echo $! >"${PID_FILE}"
echo "proxy started pid=$(cat "${PID_FILE}") log=${LOG_FILE}"
