#!/usr/bin/env bash
set -euo pipefail

if ! command -v smbclient >/dev/null 2>&1; then
    echo "SKIP: smbclient is not installed"
    exit 77
fi

: "${YUAN_SMB_HOST:=127.0.0.1}"
: "${YUAN_SMB_PORT:=445}"
: "${YUAN_SMB_DOMAIN:=WORKGROUP}"
: "${YUAN_SMBCLIENT_SIGNING:=default}"
: "${YUAN_SMBCLIENT_DEBUGLEVEL:=0}"

if [[ -z "${YUAN_SMB_SHARE:-}" || -z "${YUAN_SMB_USER:-}" || -z "${YUAN_SMB_PASSWORD:-}" ]]; then
    echo "SKIP: set YUAN_SMB_SHARE, YUAN_SMB_USER, and YUAN_SMB_PASSWORD for live NAS SMB smoke"
    exit 77
fi

if [[ -n "${YUAN_NAS_HEALTH_URL:-}" ]] && command -v curl >/dev/null 2>&1; then
    health_body="$(curl -fsSL "$YUAN_NAS_HEALTH_URL" || true)"
    if [[ -n "$health_body" && "$health_body" != *'"smb_enabled":true'* ]]; then
        echo "FAIL: NAS health does not report smb_enabled=true"
        echo "$health_body"
        exit 1
    fi
fi

script_dir="$(cd "$(dirname "$0")" && pwd)"
YUAN_SMB_USE_FIXTURE=0 \
YUAN_SMB_HOST="$YUAN_SMB_HOST" \
YUAN_SMB_PORT="$YUAN_SMB_PORT" \
YUAN_SMB_DOMAIN="$YUAN_SMB_DOMAIN" \
YUAN_SMB_SHARE="$YUAN_SMB_SHARE" \
YUAN_SMB_USER="$YUAN_SMB_USER" \
YUAN_SMB_PASSWORD="$YUAN_SMB_PASSWORD" \
YUAN_SMBCLIENT_SIGNING="$YUAN_SMBCLIENT_SIGNING" \
YUAN_SMBCLIENT_DEBUGLEVEL="$YUAN_SMBCLIENT_DEBUGLEVEL" \
bash "$script_dir/smbclient_nas_smoke.sh"
