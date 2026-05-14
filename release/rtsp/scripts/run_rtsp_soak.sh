#!/usr/bin/env bash
set -euo pipefail

build_dir="${1:-${YUAN_BUILD_DIR:-}}"
duration_sec="${2:-3600}"
parallel="${3:-4}"
max_failures="${4:-1}"
regex="${5:-rtsp}"
out_dir="${6:-./logs/rtsp_soak}"

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
driver_log="$out_dir/driver-$ts.log"
jsonl_path="$out_dir/iterations-$ts.jsonl"
summary_path="$out_dir/summary-$ts.json"

echo "[$(date -Iseconds)] start rtsp soak" | tee -a "$driver_log"
echo "build_dir=$build_dir duration_sec=$duration_sec parallel=$parallel regex=$regex max_failures=$max_failures" | tee -a "$driver_log"

ctest --test-dir "$build_dir" -N -R "$regex" 2>&1 | tee -a "$driver_log"

start_epoch="$(date +%s)"
end_epoch="$((start_epoch + duration_sec))"

iteration=0
pass_count=0
fail_count=0
elapsed_total_ms=0
aborted_by_failure=false

while [[ "$(date +%s)" -lt "$end_epoch" ]]; do
  iteration="$((iteration + 1))"
  iter_start_ms="$(date +%s%3N 2>/dev/null || python - <<'PY'
import time
print(int(time.time()*1000))
PY
)"

  if ctest --test-dir "$build_dir" -R "$regex" --output-on-failure -j "$parallel" 2>&1 | tee -a "$driver_log"; then
    exit_code=0
    pass_count="$((pass_count + 1))"
  else
    exit_code=$?
    fail_count="$((fail_count + 1))"
  fi

  iter_end_ms="$(date +%s%3N 2>/dev/null || python - <<'PY'
import time
print(int(time.time()*1000))
PY
)"
  elapsed_ms="$((iter_end_ms - iter_start_ms))"
  elapsed_total_ms="$((elapsed_total_ms + elapsed_ms))"

  printf '{"iteration":%d,"elapsed_ms":%d,"exit_code":%d}\n' "$iteration" "$elapsed_ms" "$exit_code" >> "$jsonl_path"
  echo "[$(date -Iseconds)] iteration=$iteration exit=$exit_code elapsed_ms=$elapsed_ms pass=$pass_count fail=$fail_count" | tee -a "$driver_log"

  if [[ "$fail_count" -ge "$max_failures" ]]; then
    aborted_by_failure=true
    echo "[$(date -Iseconds)] stop early fail_count=$fail_count max_failures=$max_failures" | tee -a "$driver_log"
    break
  fi
done

if [[ "$iteration" -gt 0 ]]; then
  avg_iteration_ms="$((elapsed_total_ms / iteration))"
else
  avg_iteration_ms=0
fi

cat > "$summary_path" <<JSON
{
  "build_dir": "$build_dir",
  "regex": "$regex",
  "duration_sec": $duration_sec,
  "parallel": $parallel,
  "iterations": $iteration,
  "pass_count": $pass_count,
  "fail_count": $fail_count,
  "aborted_by_failure": $aborted_by_failure,
  "avg_iteration_ms": $avg_iteration_ms,
  "driver_log": "$driver_log",
  "iterations_jsonl": "$jsonl_path"
}
JSON

echo "[$(date -Iseconds)] rtsp soak done" | tee -a "$driver_log"
echo "summary=$summary_path" | tee -a "$driver_log"

if [[ "$fail_count" -gt 0 ]]; then
  exit 1
fi
