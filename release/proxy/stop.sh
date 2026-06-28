#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PID_FILE="${YUAN_PROXY_PID_FILE:-${SCRIPT_DIR}/proxy.pid}"
TIMEOUT_SEC="${YUAN_PROXY_STOP_TIMEOUT_SEC:-10}"

if [[ ! -f "${PID_FILE}" ]]; then
  echo "proxy pid file not found"
  exit 0
fi

PID="$(cat "${PID_FILE}")"
if ! kill -0 "${PID}" 2>/dev/null; then
  rm -f "${PID_FILE}"
  echo "proxy not running"
  exit 0
fi

kill -TERM "${PID}" 2>/dev/null || true
for ((i = 0; i < TIMEOUT_SEC * 10; ++i)); do
  if ! kill -0 "${PID}" 2>/dev/null; then
    rm -f "${PID_FILE}"
    echo "proxy stopped"
    exit 0
  fi
  sleep 0.1
done

kill -KILL "${PID}" 2>/dev/null || true
rm -f "${PID_FILE}"
echo "proxy force stopped"
