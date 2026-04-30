#!/usr/bin/env bash
set -euo pipefail

if [[ "${YUAN_RUN_SSH_CLI_PROCESS_PROBE:-0}" != "1" ]]; then
  echo "skip: set YUAN_RUN_SSH_CLI_PROCESS_PROBE=1 to run bootstrap probe"
  exit 0
fi

ROOT_DIR="$(cd "$(dirname "$0")/../../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
TMP_DIR="${ROOT_DIR}/build/.tmp_ssh_cli_probe"
mkdir -p "${TMP_DIR}"

if [[ -z "${YUAN_SSH_PROBE_HOST:-}" || -z "${YUAN_SSH_PROBE_PORT:-}" || -z "${YUAN_SSH_PROBE_USER:-}" || -z "${YUAN_SSH_PROBE_KEY:-}" ]]; then
  echo "missing env vars: YUAN_SSH_PROBE_HOST/PORT/USER/KEY"
  exit 1
fi

"${BUILD_DIR}/libs/ssh_cli/test_ssh_cli_process_probe"

if [[ "${YUAN_RUN_SSH_CLI_LOCAL:-0}" == "1" ]]; then
  "${BUILD_DIR}/libs/ssh_cli/test_ssh_cli_process_local"
fi
