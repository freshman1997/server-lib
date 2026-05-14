#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../../../../" && pwd)"
build_dir="${YUAN_BUILD_DIR:-}"

if [[ -z "$build_dir" ]]; then
  if [[ -d "$repo_root/build-mingw" ]]; then
    build_dir="$repo_root/build-mingw"
  else
    build_dir="$repo_root/build"
  fi
fi

if [[ ! -d "$build_dir" ]]; then
  echo "FAIL: build dir not found: $build_dir"
  exit 1
fi

echo "==> Build RTSP/RTCP test binaries"
cmake --build "$build_dir" --target test_rtsp test_rtsp_server test_rtsp_interop test_rtcp test_rtcp_session test_rtcp_loopback

echo "==> Run RTSP/RTCP gate"
bash "$repo_root/release/rtsp/scripts/run_rtsp_gate.sh" "$build_dir" "rtsp|rtcp"

echo "RTSP preflight complete."
