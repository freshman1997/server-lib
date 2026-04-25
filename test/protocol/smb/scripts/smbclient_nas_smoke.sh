#!/usr/bin/env bash
set -euo pipefail

if ! command -v smbclient >/dev/null 2>&1; then
    echo "SKIP: smbclient is not installed"
    exit 77
fi

: "${YUAN_SMB_HOST:=127.0.0.1}"
: "${YUAN_SMB_PORT:=445}"
: "${YUAN_SMB_DOMAIN:=WORKGROUP}"

if [[ -z "${YUAN_SMB_SHARE:-}" || -z "${YUAN_SMB_USER:-}" || -z "${YUAN_SMB_PASSWORD:-}" ]]; then
    echo "SKIP: set YUAN_SMB_SHARE, YUAN_SMB_USER, and YUAN_SMB_PASSWORD to run smbclient NAS smoke"
    exit 77
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

local_file="$tmpdir/yuan_smb_smoke.txt"
downloaded_file="$tmpdir/yuan_smb_smoke.downloaded.txt"
remote_file="yuan_smb_smoke_$$.txt"
renamed_file="yuan_smb_smoke_renamed_$$.txt"

printf 'yuan smb smoke %s\n' "$$" > "$local_file"

smbclient "//$YUAN_SMB_HOST/$YUAN_SMB_SHARE" \
    -p "$YUAN_SMB_PORT" \
    -W "$YUAN_SMB_DOMAIN" \
    -U "$YUAN_SMB_USER%$YUAN_SMB_PASSWORD" \
    -m SMB3 \
    -c "put \"$local_file\" \"$remote_file\"; get \"$remote_file\" \"$downloaded_file\"; rename \"$remote_file\" \"$renamed_file\"; del \"$renamed_file\"; ls" >/tmp/yuan_smbclient_smoke.log

cmp "$local_file" "$downloaded_file"
echo "PASS: smbclient NAS smoke completed"
