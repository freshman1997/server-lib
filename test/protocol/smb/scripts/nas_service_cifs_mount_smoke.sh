#!/usr/bin/env bash
set -euo pipefail

if ! command -v mount.cifs >/dev/null 2>&1; then
    echo "SKIP: mount.cifs is not installed"
    exit 77
fi

if ! command -v umount >/dev/null 2>&1; then
    echo "SKIP: umount is not available"
    exit 77
fi

: "${YUAN_SMB_HOST:=127.0.0.1}"
: "${YUAN_SMB_PORT:=445}"
: "${YUAN_SMB_SHARE:=}"
: "${YUAN_SMB_USER:=}"
: "${YUAN_SMB_PASSWORD:=}"
: "${YUAN_SMB_DOMAIN:=WORKGROUP}"
: "${YUAN_CIFS_VERS:=3.1.1}"
: "${YUAN_CIFS_SIGNING:=default}"

if [[ -z "$YUAN_SMB_SHARE" || -z "$YUAN_SMB_USER" || -z "$YUAN_SMB_PASSWORD" ]]; then
    echo "SKIP: set YUAN_SMB_SHARE, YUAN_SMB_USER, and YUAN_SMB_PASSWORD for CIFS smoke"
    exit 77
fi

mount_cmd=()
if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
    if command -v sudo >/dev/null 2>&1 && sudo -n true >/dev/null 2>&1; then
        mount_cmd=(sudo -n)
    else
        echo "SKIP: CIFS mount requires root or passwordless sudo"
        exit 77
    fi
fi

tmpdir="$(mktemp -d)"
mount_point="$tmpdir/mnt"
cred_file="$tmpdir/cred"
mkdir -p "$mount_point"

mounted=0
cleanup() {
    if [[ "$mounted" -eq 1 ]]; then
        "${mount_cmd[@]}" umount "$mount_point" >/dev/null 2>&1 || true
    fi
    rm -rf "$tmpdir"
}
trap cleanup EXIT

cat >"$cred_file" <<EOF
username=$YUAN_SMB_USER
password=$YUAN_SMB_PASSWORD
domain=$YUAN_SMB_DOMAIN
EOF
chmod 600 "$cred_file"

mount_opts="credentials=$cred_file,vers=$YUAN_CIFS_VERS,port=$YUAN_SMB_PORT,iocharset=utf8"
case "${YUAN_CIFS_SIGNING,,}" in
    default|auto)
        ;;
    required)
        mount_opts="$mount_opts,sign"
        ;;
    *)
        echo "SKIP: unsupported YUAN_CIFS_SIGNING value: $YUAN_CIFS_SIGNING"
        exit 77
        ;;
esac

"${mount_cmd[@]}" mount -t cifs "//$YUAN_SMB_HOST/$YUAN_SMB_SHARE" "$mount_point" -o "$mount_opts"
mounted=1

local_src="$tmpdir/local.txt"
remote_file="yuan_cifs_smoke_$$.txt"
renamed_file="yuan_cifs_smoke_renamed_$$.txt"
local_copy="$tmpdir/copied.txt"

printf 'yuan cifs smoke %s\n' "$$" > "$local_src"

cp "$local_src" "$mount_point/$remote_file"
cp "$mount_point/$remote_file" "$local_copy"
cmp "$local_src" "$local_copy"
mv "$mount_point/$remote_file" "$mount_point/$renamed_file"
rm -f "$mount_point/$renamed_file"
ls "$mount_point" >/dev/null

echo "PASS: NAS CIFS mount smoke completed"
