#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BUILD_DIR="${ROOT_DIR}/build"
OUTPUT="${ROOT_DIR}/docs/benchmarks/game_benchmark_soak.csv"

BACKEND="epoll_affinity"
WORKERS=4
CONNECTIONS=10000
PPS=50
COMPLETION_BATCH=1
FRAME_SIZE=32
CLIENT_THREADS=16

TOTAL_DURATION_SEC=43200
SEGMENT_DURATION_SEC=300
SAMPLE_INTERVAL_SEC=1
RESUME=1

STRICT_ARGS=()

usage() {
    printf '%s\n' "Usage: scripts/bench_game_soak.sh [options]"
    printf '%s\n' ""
    printf '%s\n' "Options:"
    printf '%s\n' "  --build-dir <path>          Build directory (default: ./build)"
    printf '%s\n' "  --output <path>             Output CSV path"
    printf '%s\n' "  --backend <name>            Backend (default: epoll_affinity)"
    printf '%s\n' "  --workers <n>               Worker count (default: 4)"
    printf '%s\n' "  --connections <n>           Connection count (default: 10000)"
    printf '%s\n' "  --pps <n>                   Packets per second per connection (default: 50)"
    printf '%s\n' "  --completion-batch <n>      Completion batch size (default: 1)"
    printf '%s\n' "  --frame-size <bytes>        Frame size (default: 32)"
    printf '%s\n' "  --client-threads <n>        Client thread count (default: 16)"
    printf '%s\n' "  --total-duration <sec>      Total soak duration (default: 43200, 12h)"
    printf '%s\n' "  --segment-duration <sec>    Per-segment duration (default: 300)"
    printf '%s\n' "  --sample-interval <sec>     Process sample interval (default: 1)"
    printf '%s\n' "  --min-pps <value>           Strict lower bound for packets_per_second"
    printf '%s\n' "  --max-avg-rtt-us <value>    Strict upper bound for avg_rtt_us"
    printf '%s\n' "  --max-p95-rtt-us <value>    Strict upper bound for p95_rtt_us"
    printf '%s\n' "  --max-p99-rtt-us <value>    Strict upper bound for p99_rtt_us"
    printf '%s\n' "  --max-max-rtt-us <value>    Strict upper bound for max_rtt_us"
    printf '%s\n' "  --no-strict                 Disable strict validation"
    printf '%s\n' "  --no-resume                 Do not resume from existing CSV"
    printf '%s\n' "  -h, --help                  Show this message"
}

is_integer() {
    [[ "$1" =~ ^[0-9]+$ ]]
}

while [[ $# -gt 0 ]]; do
    case "$1" in
    --build-dir)
        BUILD_DIR="$2"
        shift 2
        ;;
    --output)
        OUTPUT="$2"
        shift 2
        ;;
    --backend)
        BACKEND="$2"
        shift 2
        ;;
    --workers)
        WORKERS="$2"
        shift 2
        ;;
    --connections)
        CONNECTIONS="$2"
        shift 2
        ;;
    --pps)
        PPS="$2"
        shift 2
        ;;
    --completion-batch)
        COMPLETION_BATCH="$2"
        shift 2
        ;;
    --frame-size)
        FRAME_SIZE="$2"
        shift 2
        ;;
    --client-threads)
        CLIENT_THREADS="$2"
        shift 2
        ;;
    --total-duration)
        TOTAL_DURATION_SEC="$2"
        shift 2
        ;;
    --segment-duration)
        SEGMENT_DURATION_SEC="$2"
        shift 2
        ;;
    --sample-interval)
        SAMPLE_INTERVAL_SEC="$2"
        shift 2
        ;;
    --min-pps)
        STRICT_ARGS+=("--min-pps" "$2")
        shift 2
        ;;
    --max-avg-rtt-us)
        STRICT_ARGS+=("--max-avg-rtt-us" "$2")
        shift 2
        ;;
    --max-p95-rtt-us)
        STRICT_ARGS+=("--max-p95-rtt-us" "$2")
        shift 2
        ;;
    --max-p99-rtt-us)
        STRICT_ARGS+=("--max-p99-rtt-us" "$2")
        shift 2
        ;;
    --max-max-rtt-us)
        STRICT_ARGS+=("--max-max-rtt-us" "$2")
        shift 2
        ;;
    --no-strict)
        STRICT_ARGS+=("--no-strict")
        shift
        ;;
    --no-resume)
        RESUME=0
        shift
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        printf 'Unknown option: %s\n\n' "$1" >&2
        usage >&2
        exit 1
        ;;
    esac
done

for n in "$WORKERS" "$CONNECTIONS" "$PPS" "$COMPLETION_BATCH" "$FRAME_SIZE" "$CLIENT_THREADS" "$TOTAL_DURATION_SEC" "$SEGMENT_DURATION_SEC"; do
    if ! is_integer "$n"; then
        printf 'Numeric option must be integer: %s\n' "$n" >&2
        exit 1
    fi
done

if [[ "$SEGMENT_DURATION_SEC" -le 0 || "$TOTAL_DURATION_SEC" -le 0 ]]; then
    printf 'Durations must be greater than zero\n' >&2
    exit 1
fi

if [[ ! -d "$BUILD_DIR" ]]; then
    printf 'Build directory not found: %s\n' "$BUILD_DIR" >&2
    exit 1
fi

segments=$(( (TOTAL_DURATION_SEC + SEGMENT_DURATION_SEC - 1) / SEGMENT_DURATION_SEC ))
output_dir="$(dirname "$OUTPUT")"
mkdir -p "$output_dir"

cmake --build "$BUILD_DIR" -j "${JOBS:-8}" --target game_benchmark

completed_segments=0
if [[ -f "$OUTPUT" && "$RESUME" -eq 1 ]]; then
    completed_segments=$(( $(wc -l < "$OUTPUT") - 1 ))
    if [[ "$completed_segments" -lt 0 ]]; then
        completed_segments=0
    fi
fi

if [[ "$RESUME" -eq 0 || ! -f "$OUTPUT" ]]; then
    completed_segments=0
    rm -f "$OUTPUT"
fi

if [[ "$completed_segments" -ge "$segments" ]]; then
    printf 'All segments already completed (%s/%s): %s\n' "$completed_segments" "$segments" "$OUTPUT"
    exit 0
fi

segment_idx=$((completed_segments + 1))
while [[ "$segment_idx" -le "$segments" ]]; do
    remaining=$((TOTAL_DURATION_SEC - (segment_idx - 1) * SEGMENT_DURATION_SEC))
    duration="$SEGMENT_DURATION_SEC"
    if [[ "$remaining" -lt "$duration" ]]; then
        duration="$remaining"
    fi

    printf 'Soak segment %s/%s backend=%s connections=%s pps=%s duration=%ss\n' \
        "$segment_idx" "$segments" "$BACKEND" "$CONNECTIONS" "$PPS" "$duration"

    tmp_csv="$(mktemp)"
    "${ROOT_DIR}/scripts/bench_game_matrix.sh" \
        --build-dir "$BUILD_DIR" \
        --skip-build \
        --backends "$BACKEND" \
        --connections "$CONNECTIONS" \
        --pps "$PPS" \
        --workers "$WORKERS" \
        --duration "$duration" \
        --completion-batch "$COMPLETION_BATCH" \
        --frame-size "$FRAME_SIZE" \
        --client-threads "$CLIENT_THREADS" \
        --sample-interval "$SAMPLE_INTERVAL_SEC" \
        --repeats 1 \
        --output "$tmp_csv" \
        "${STRICT_ARGS[@]}"

    if [[ ! -f "$OUTPUT" ]]; then
        cp "$tmp_csv" "$OUTPUT"
    else
        awk 'NR > 1 { print }' "$tmp_csv" >> "$OUTPUT"
    fi
    rm -f "$tmp_csv"

    segment_idx=$((segment_idx + 1))
done

printf 'Soak done. Wrote CSV: %s\n' "$OUTPUT"
