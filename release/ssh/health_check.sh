#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
SERVER_BIN="${SERVER_BIN:-${BUILD_DIR}/release/ssh/release_ssh_server}"
CLI_BIN="${CLI_BIN:-${BUILD_DIR}/release/ssh/release_ssh_cli}"

SSH_HOST="${SSH_HOST:-127.0.0.1}"
SSH_PORT="${SSH_PORT:-2222}"
SSH_USER="${SSH_USER:-yuan}"
SSH_KEY_PATH="${SSH_KEY_PATH:-}"

LOG_FILE="${LOG_FILE:-/tmp/release_ssh_server.log}"
KNOWN_HOSTS_FILE="${KNOWN_HOSTS_FILE:-/tmp/yuan_ssh_known_hosts_health}"

echo "[1/5] Checking binaries"
[[ -x "${SERVER_BIN}" ]]
[[ -x "${CLI_BIN}" ]]

echo "[2/5] Checking server process"
if ! pgrep -af "release_ssh_server" >/dev/null; then
  echo "release_ssh_server is not running"
  exit 1
fi

echo "[3/5] Checking port listen (2222)"
ss -ltnp | grep ":${SSH_PORT}" >/dev/null

echo "[4/5] Checking version exchange via bundled CLI"
"${CLI_BIN}" --probe --host "${SSH_HOST}" --port "${SSH_PORT}" --timeout-ms 3000 >/dev/null

echo "[5/5] Checking bundled CLI exec path"
"${CLI_BIN}" -p "${SSH_PORT}" --password "${SSH_PASSWORD:-yuan}" "${SSH_USER}@${SSH_HOST}" "whoami" >/dev/null

echo "[optional] Checking OpenSSH command path"
if [[ -n "${SSH_USER}" && -n "${SSH_KEY_PATH}" && -f "${SSH_KEY_PATH}" ]]; then
  ssh -o BatchMode=yes \
      -o StrictHostKeyChecking=no \
      -o UserKnownHostsFile="${KNOWN_HOSTS_FILE}" \
      -i "${SSH_KEY_PATH}" \
      -p "${SSH_PORT}" "${SSH_USER}@${SSH_HOST}" "whoami" >/dev/null
  echo "ssh publickey command path ok"
else
  echo "skip ssh command path: set SSH_USER and SSH_KEY_PATH to enable"
fi

echo "health check passed"
echo "recent server log: ${LOG_FILE}"
