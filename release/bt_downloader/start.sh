#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${SERVER_BIN:-${SCRIPT_DIR}/bt_downloader}"
CONFIG="${YUAN_BT_CONFIG:-${SCRIPT_DIR}/config.json}"
PID_FILE="${PID_FILE:-${SCRIPT_DIR}/bt_downloader.pid}"
LOG_FILE="${LOG_FILE:-${SCRIPT_DIR}/bt_downloader.log}"

if [[ ! -x "${BIN}" ]]; then
  echo "server binary not found or not executable: ${BIN}" >&2
  exit 1
fi

if [[ -f "${PID_FILE}" ]]; then
  pid="$(cat "${PID_FILE}")"
  if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
    echo "bt_downloader is already running: ${pid}"
    exit 0
  fi
  rm -f "${PID_FILE}"
fi

mkdir -p "$(dirname "${LOG_FILE}")"
nohup "${BIN}" "${CONFIG}" "$@" >"${LOG_FILE}" 2>&1 &
echo "$!" >"${PID_FILE}"
echo "bt_downloader started: $(cat "${PID_FILE}")"
echo "log: ${LOG_FILE}"
