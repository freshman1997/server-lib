#!/usr/bin/env bash
set -euo pipefail

build_dir="${1:-${YUAN_BUILD_DIR:-}}"
regex="${2:-rtsp_server|rtsp_state_matrix|rtsp_interop}"
levels="${3:-2,4,8}"
rounds="${4:-3}"
out_dir="${5:-./logs/rtsp_concurrency}"

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
log="$out_dir/concurrency-$ts.log"
json="$out_dir/concurrency-$ts.json"

IFS=',' read -r -a parr <<< "$levels"
echo "[" > "$json"
first=1
for p in "${parr[@]}"; do
  for ((r=1; r<=rounds; r++)); do
    start_ms="$(date +%s%3N 2>/dev/null || python - <<'PY'
import time
print(int(time.time()*1000))
PY
)"
    if ctest --test-dir "$build_dir" -R "$regex" --output-on-failure -j "$p" 2>&1 | tee -a "$log"; then
      code=0
    else
      code=$?
    fi
    end_ms="$(date +%s%3N 2>/dev/null || python - <<'PY'
import time
print(int(time.time()*1000))
PY
)"
    elapsed_ms="$((end_ms - start_ms))"
    echo "[$(date -Iseconds)] parallel=$p round=$r exit=$code elapsed_ms=$elapsed_ms" | tee -a "$log"

    if [[ $first -eq 0 ]]; then
      echo "," >> "$json"
    fi
    first=0
    printf '{"parallel":%d,"round":%d,"exit_code":%d,"elapsed_ms":%d}' "$p" "$r" "$code" "$elapsed_ms" >> "$json"

    if [[ $code -ne 0 ]]; then
      echo "]" >> "$json"
      echo "FAIL: concurrency run failed parallel=$p round=$r" | tee -a "$log"
      exit 1
    fi
  done
done
echo "]" >> "$json"
echo "report=$json" | tee -a "$log"
