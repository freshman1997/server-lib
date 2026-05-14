#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
SERVER_BIN="${SERVER_BIN:-${BUILD_DIR}/release/dns/release_dns_server}"
DNS_PORT="${DNS_PORT:-5353}"

echo "[1/3] Checking binary"
[[ -x "${SERVER_BIN}" ]]

echo "[2/3] Checking server process"
if ! pgrep -af "release_dns_server" >/dev/null; then
  echo "release_dns_server is not running"
  exit 1
fi

echo "[3/3] Checking UDP listen port (${DNS_PORT})"
ss -lunp | grep ":${DNS_PORT}" >/dev/null

echo "health check passed"
