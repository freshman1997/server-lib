#!/usr/bin/env bash
set -euo pipefail

HOST="${YUAN_PROXY_HEALTH_HOST:-127.0.0.1}"
PORT="${YUAN_PROXY_LISTEN_PORT:-3128}"
TIMEOUT="${YUAN_PROXY_HEALTH_TIMEOUT_SEC:-2}"

if command -v curl >/dev/null 2>&1; then
  curl --max-time "${TIMEOUT}" -x "http://${HOST}:${PORT}" -s -o /dev/null http://example.com/ || exit 1
  echo "proxy health ok"
  exit 0
fi

python3 - "${HOST}" "${PORT}" "${TIMEOUT}" <<'PY'
import socket, sys
host, port, timeout = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
with socket.create_connection((host, port), timeout=timeout):
    pass
print("proxy tcp health ok")
PY
