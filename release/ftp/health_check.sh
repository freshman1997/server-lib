#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
SERVER_BIN="${SERVER_BIN:-${BUILD_DIR}/release/ftp/release_ftp_server}"
CLI_BIN="${CLI_BIN:-${BUILD_DIR}/release/ftp/release_ftp_cli}"

FTP_HOST="${FTP_HOST:-127.0.0.1}"
FTP_PORT="${FTP_PORT:-2121}"
FTP_USER="${FTP_USER:-tester}"
FTP_PASSWORD="${FTP_PASSWORD:-secret}"

LOG_FILE="${LOG_FILE:-/tmp/release_ftp_server.log}"

echo "[1/4] Checking binaries"
[[ -x "${SERVER_BIN}" ]]
[[ -x "${CLI_BIN}" ]]

echo "[2/4] Checking server process"
if ! pgrep -af "release_ftp_server" >/dev/null; then
  echo "release_ftp_server is not running"
  exit 1
fi

echo "[3/4] Checking port listen (${FTP_PORT})"
ss -ltnp | grep ":${FTP_PORT}" >/dev/null

echo "[4/4] Checking bundled CLI list command"
"${CLI_BIN}" --host "${FTP_HOST}" -p "${FTP_PORT}" -u "${FTP_USER}" --password "${FTP_PASSWORD}" --list >/dev/null

echo "health check passed"
echo "recent server log: ${LOG_FILE}"