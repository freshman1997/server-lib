#!/usr/bin/env bash
set -euo pipefail

build_dir="${1:-${YUAN_BUILD_DIR:-}}"
regex="${2:-rtsp|rtcp}"
out_dir="${3:-./logs/rtsp_gate}"

if [[ -z "$build_dir" ]]; then
  if [[ -d "build-mingw" ]]; then
    build_dir="build-mingw"
  else
    build_dir="build"
  fi
fi

if [[ ! -d "$build_dir" ]]; then
  echo "FAIL: build dir not found: $build_dir"
  exit 1
fi

mkdir -p "$out_dir"
ts="$(date +%Y%m%d-%H%M%S)"
log_file="$out_dir/rtsp-gate-$ts.log"

echo "[$(date -Iseconds)] build_dir=$build_dir regex=$regex" | tee -a "$log_file"

if ! ctest --test-dir "$build_dir" -R "$regex" --output-on-failure 2>&1 | tee -a "$log_file"; then
  echo "[$(date -Iseconds)] gate failed log=$log_file" | tee -a "$log_file"
  exit 1
fi

echo "[$(date -Iseconds)] gate passed log=$log_file" | tee -a "$log_file"
