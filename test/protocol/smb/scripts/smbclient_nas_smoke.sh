#!/usr/bin/env bash
set -euo pipefail

fixture_pid=""
fixture_root=""
fixture_ready_file=""

cleanup() {
    if [[ -n "$fixture_pid" ]] && kill -0 "$fixture_pid" >/dev/null 2>&1; then
        kill "$fixture_pid" >/dev/null 2>&1 || true
        wait "$fixture_pid" >/dev/null 2>&1 || true
    fi
    if [[ -n "$fixture_root" && -d "$fixture_root" ]]; then
        rm -rf "$fixture_root"
    fi
}

trap cleanup EXIT

if ! command -v smbclient >/dev/null 2>&1; then
    echo "SKIP: smbclient is not installed"
    exit 77
fi

: "${YUAN_SMB_HOST:=127.0.0.1}"
: "${YUAN_SMB_PORT:=445}"
: "${YUAN_SMB_DOMAIN:=WORKGROUP}"
: "${YUAN_SMBCLIENT_SIGNING:=default}"
: "${YUAN_SMBCLIENT_DEBUGLEVEL:=0}"

signing_arg=()
case "${YUAN_SMBCLIENT_SIGNING,,}" in
    default|auto)
        ;;
    required)
        help_text="$(smbclient --help 2>&1 || true)"
        if [[ "$help_text" == *"--signing"* ]]; then
            signing_arg=("--signing=required")
        elif [[ "$help_text" == *"--client-protection"* ]]; then
            signing_arg=("--client-protection=sign")
        else
            echo "SKIP: smbclient does not support required-signing options"
            exit 77
        fi
        ;;
    *)
        echo "SKIP: unsupported YUAN_SMBCLIENT_SIGNING value: $YUAN_SMBCLIENT_SIGNING"
        exit 77
        ;;
esac

if [[ "${YUAN_SMB_USE_FIXTURE:-0}" == "1" ]]; then
    if [[ -z "${YUAN_SMB_FIXTURE_BIN:-}" ]]; then
        echo "SKIP: set YUAN_SMB_FIXTURE_BIN when YUAN_SMB_USE_FIXTURE=1"
        exit 77
    fi
    if [[ ! -x "$YUAN_SMB_FIXTURE_BIN" ]]; then
        echo "SKIP: fixture binary is not executable: $YUAN_SMB_FIXTURE_BIN"
        exit 77
    fi

    fixture_root="$(mktemp -d)"
    fixture_ready_file="$fixture_root/ready.flag"

    : "${YUAN_SMB_SHARE:=public}"
    : "${YUAN_SMB_USER:=fixture}"
    : "${YUAN_SMB_PASSWORD:=fixture-secret}"

    "$YUAN_SMB_FIXTURE_BIN" "$fixture_root" "$YUAN_SMB_SHARE" "$YUAN_SMB_USER" "$YUAN_SMB_PASSWORD" "$YUAN_SMB_PORT" "$fixture_ready_file" >/tmp/yuan_smb_fixture.log 2>&1 &
    fixture_pid=$!

    for _ in $(seq 1 50); do
        if [[ -f "$fixture_ready_file" ]]; then
            break
        fi
        if ! kill -0 "$fixture_pid" >/dev/null 2>&1; then
            echo "FAIL: SMB fixture exited early"
            if [[ -f /tmp/yuan_smb_fixture.log ]]; then
                cat /tmp/yuan_smb_fixture.log
            fi
            exit 1
        fi
        sleep 0.1
    done

    if [[ ! -f "$fixture_ready_file" ]]; then
        echo "FAIL: SMB fixture did not become ready"
        if [[ -f /tmp/yuan_smb_fixture.log ]]; then
            cat /tmp/yuan_smb_fixture.log
        fi
        exit 1
    fi
elif [[ -z "${YUAN_SMB_SHARE:-}" || -z "${YUAN_SMB_USER:-}" || -z "${YUAN_SMB_PASSWORD:-}" ]]; then
    echo "SKIP: set YUAN_SMB_SHARE, YUAN_SMB_USER, and YUAN_SMB_PASSWORD to run smbclient NAS smoke, or enable YUAN_SMB_USE_FIXTURE=1"
    exit 77
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"; cleanup' EXIT

local_file="$tmpdir/yuan_smb_smoke.txt"
downloaded_file="$tmpdir/yuan_smb_smoke.downloaded.txt"
remote_file="yuan_smb_smoke_$$.txt"
renamed_file="yuan_smb_smoke_renamed_$$.txt"

printf 'yuan smb smoke %s\n' "$$" > "$local_file"

smbclient "//$YUAN_SMB_HOST/$YUAN_SMB_SHARE" \
    -p "$YUAN_SMB_PORT" \
    -W "$YUAN_SMB_DOMAIN" \
    -U "$YUAN_SMB_USER%$YUAN_SMB_PASSWORD" \
    "${signing_arg[@]}" \
    -m SMB3 \
    -d "$YUAN_SMBCLIENT_DEBUGLEVEL" \
    -c "put \"$local_file\" \"$remote_file\"; get \"$remote_file\" \"$downloaded_file\"; rename \"$remote_file\" \"$renamed_file\"; del \"$renamed_file\"; ls" >/tmp/yuan_smbclient_smoke.log 2>&1

cmp "$local_file" "$downloaded_file"
echo "PASS: smbclient NAS smoke completed"
