#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BIN_SERVER="$ROOT_DIR/build/release/filesync/release_filesync_server"
BIN_CLIENT="$ROOT_DIR/build/release/filesync/release_filesync_client"

if [[ ! -x "$BIN_SERVER" || ! -x "$BIN_CLIENT" ]]; then
  echo "missing binaries, build first:" >&2
  echo "  cmake --build \"$ROOT_DIR/build\" --target release_filesync_server release_filesync_client" >&2
  exit 1
fi

TMP_DIR="$(mktemp -d)"
SERVER_DIR="$TMP_DIR/server"
CLIENT_DIR="$TMP_DIR/client"
SERVER_CFG="$TMP_DIR/server.json"
CLIENT_CFG="$TMP_DIR/client.json"
SERVER_LOG="$TMP_DIR/server.log"
CLIENT_LOG="$TMP_DIR/client.log"

mkdir -p "$SERVER_DIR" "$CLIENT_DIR"
printf 'from-server\n' > "$SERVER_DIR/server.txt"
printf 'from-client\n' > "$CLIENT_DIR/client.txt"
dd if=/dev/urandom of="$CLIENT_DIR/big.bin" bs=1M count=3 status=none

cat > "$SERVER_CFG" <<EOF
{"listen_host":"127.0.0.1","listen_port":29995,"peer_host":"","peer_port":29996,"token":"test-token","conflict_strategy":"newer_wins","sync_deletes":true,"scan_interval_ms":500,"chunk_size":32768,"include_extensions":[],"include_patterns":[],"exclude_patterns":[],"paths":[{"local":"$SERVER_DIR","remote_prefix":"work"}]}
EOF

cat > "$CLIENT_CFG" <<EOF
{"listen_host":"127.0.0.1","listen_port":29996,"peer_host":"127.0.0.1","peer_port":29995,"token":"test-token","conflict_strategy":"newer_wins","sync_deletes":true,"scan_interval_ms":500,"chunk_size":32768,"include_extensions":[],"include_patterns":[],"exclude_patterns":[],"paths":[{"local":"$CLIENT_DIR","remote_prefix":"work"}]}
EOF

cleanup() {
  kill "$CLIENT_PID" "$SERVER_PID" 2>/dev/null || true
  wait "$CLIENT_PID" "$SERVER_PID" 2>/dev/null || true
}

"$BIN_SERVER" "$SERVER_CFG" > "$SERVER_LOG" 2>&1 &
SERVER_PID=$!
"$BIN_CLIENT" "$CLIENT_CFG" > "$CLIENT_LOG" 2>&1 &
CLIENT_PID=$!

trap cleanup EXIT

for _ in $(seq 1 180); do
  if [[ -f "$SERVER_DIR/client.txt" && -f "$CLIENT_DIR/server.txt" && -f "$SERVER_DIR/big.bin" ]]; then
    break
  fi
  sleep 0.2
done

cmp "$CLIENT_DIR/big.bin" "$SERVER_DIR/big.bin"

rm -f "$CLIENT_DIR/server.txt"
for _ in $(seq 1 180); do
  if [[ ! -e "$SERVER_DIR/server.txt" ]]; then
    break
  fi
  sleep 0.2
done

if [[ -e "$SERVER_DIR/server.txt" ]]; then
  echo "delete sync failed" >&2
  exit 1
fi

if ! kill -0 "$CLIENT_PID" 2>/dev/null; then
  echo "client process exited unexpectedly" >&2
  exit 1
fi

echo "filesync local validation OK"
echo "tmp logs: $TMP_DIR"
