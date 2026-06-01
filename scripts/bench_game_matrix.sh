#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BUILD_DIR="${ROOT_DIR}/build"
BIN_REL="test/benchmark/game_benchmark"
WORKERS=4
DURATION=10
COMPLETION_BATCH=1
FRAME_SIZE=32
CLIENT_THREADS=16
REPEATS=1
SKIP_BUILD=0
STRICT=1
SAMPLE_INTERVAL_SEC=1
MIN_PPS=""
MAX_AVG_RTT_US=""
MAX_P95_RTT_US=""
MAX_P99_RTT_US=""
MAX_MAX_RTT_US=""

BACKENDS_CSV="epoll,epoll_affinity"
CONNECTIONS_CSV="1000,10000,50000"
PPS_CSV="20,50,100"

OUTPUT=""

usage() {
    printf '%s\n' "Usage: scripts/bench_game_matrix.sh [options]"
    printf '%s\n' ""
    printf '%s\n' "Options:"
    printf '%s\n' "  --build-dir <path>          Build directory (default: ./build)"
    printf '%s\n' "  --bin <relative-path>       Binary path relative to build dir (default: test/benchmark/game_benchmark)"
    printf '%s\n' "  --workers <n>               Server worker count (default: 4)"
    printf '%s\n' "  --duration <sec>            Benchmark duration in seconds (default: 10)"
    printf '%s\n' "  --completion-batch <n>      Completion batch size (default: 1)"
    printf '%s\n' "  --frame-size <bytes>        Frame size (default: 32)"
    printf '%s\n' "  --client-threads <n>        Client thread count (default: 16)"
    printf '%s\n' "  --backends <csv>            Backends csv (default: epoll,epoll_affinity)"
    printf '%s\n' "  --connections <csv>         Connections csv (default: 1000,10000,50000)"
    printf '%s\n' "  --pps <csv>                 Packets-per-second csv (default: 20,50,100)"
    printf '%s\n' "  --repeats <n>               Repeat count per case (default: 1)"
    printf '%s\n' "  --output <path>             Output CSV path"
    printf '%s\n' "  --sample-interval <sec>     Process sample interval seconds (default: 1)"
    printf '%s\n' "  --min-pps <value>           Strict lower bound for packets_per_second"
    printf '%s\n' "  --max-avg-rtt-us <value>    Strict upper bound for avg_rtt_us"
    printf '%s\n' "  --max-p95-rtt-us <value>    Strict upper bound for p95_rtt_us"
    printf '%s\n' "  --max-p99-rtt-us <value>    Strict upper bound for p99_rtt_us"
    printf '%s\n' "  --max-max-rtt-us <value>    Strict upper bound for max_rtt_us"
    printf '%s\n' "  --no-strict                 Do not fail on runtime mismatches"
    printf '%s\n' "  --skip-build                Skip cmake build step"
    printf '%s\n' "  -h, --help                  Show this message"
}

split_csv() {
    local csv="$1"
    local -n out_ref="$2"
    out_ref=()
    IFS=',' read -r -a out_ref <<< "$csv"
}

extract_header_value() {
    local line="$1"
    local key="$2"
    local token
    for token in $line; do
        if [[ "$token" == "$key="* ]]; then
            printf '%s' "${token#*=}"
            return 0
        fi
    done
    printf '%s' ""
}

extract_result_value() {
    local line="$1"
    local key="$2"
    local token
    for token in $line; do
        if [[ "$token" == "$key="* ]]; then
            printf '%s' "${token#*=}"
            return 0
        fi
        if [[ "$token" == "$key<="* ]]; then
            printf '%s' "${token#*<=}"
            return 0
        fi
    done
    printf '%s' ""
}

require_non_empty() {
    local value="$1"
    local label="$2"
    if [[ -z "$value" ]]; then
        printf 'Missing parsed field: %s\n' "$label" >&2
        exit 1
    fi
}

is_number() {
    local value="$1"
    [[ "$value" =~ ^[0-9]+([.][0-9]+)?$ ]]
}

extract_status_number() {
    local pid="$1"
    local key="$2"
    local status_file="/proc/${pid}/status"
    if [[ ! -r "$status_file" ]]; then
        printf '%s' ""
        return 0
    fi
    while IFS=':' read -r k v; do
        if [[ "$k" == "$key" ]]; then
            set -- $v
            printf '%s' "${1:-}"
            return 0
        fi
    done < "$status_file"
    printf '%s' ""
}

extract_proc_jiffies() {
    local pid="$1"
    local stat_file="/proc/${pid}/stat"
    if [[ ! -r "$stat_file" ]]; then
        printf '%s' ""
        return 0
    fi
    local line rest
    IFS= read -r line < "$stat_file" || {
        printf '%s' ""
        return 0
    }
    rest="${line##*) }"
    read -r -a fields <<< "$rest"
    if [[ ${#fields[@]} -lt 13 ]]; then
        printf '%s' ""
        return 0
    fi
    printf '%s' "$((fields[11] + fields[12]))"
}

extract_io_counter() {
    local pid="$1"
    local key="$2"
    local io_file="/proc/${pid}/io"
    if [[ ! -r "$io_file" ]]; then
        printf '%s' ""
        return 0
    fi
    while IFS=':' read -r k v; do
        if [[ "$k" == "$key" ]]; then
            set -- $v
            printf '%s' "${1:-}"
            return 0
        fi
    done < "$io_file"
    printf '%s' ""
}

compute_float() {
    local expr="$1"
    awk "BEGIN { printf \"%.6f\", (${expr}) }"
}

run_one_case() {
    local backend="$1"
    local connections="$2"
    local pps="$3"
    local effective_client_threads="$4"

    local tmp_output
    tmp_output="$(mktemp)"

    local start_epoch end_epoch elapsed_sec
    local hz
    hz="$(getconf CLK_TCK)"
    start_epoch="$(date +%s.%N)"

    "$BIN_PATH" \
        "$WORKERS" \
        "$DURATION" \
        "$connections" \
        "$pps" \
        "$COMPLETION_BATCH" \
        "$FRAME_SIZE" \
        "$effective_client_threads" \
        "$backend" > "$tmp_output" 2>&1 &
    local bench_pid=$!

    local start_jiffies end_jiffies
    local rss_peak_kb=0
    local fd_peak=0
    local ctx_vol_start ctx_nonvol_start
    local ctx_vol_end ctx_nonvol_end
    local ctx_vol_delta=0
    local ctx_nonvol_delta=0
    local syscr_start syscw_start
    local syscr_end syscw_end
    local syscr_last syscw_last
    local syscr_delta=0
    local syscw_delta=0

    start_jiffies="$(extract_proc_jiffies "$bench_pid")"
    ctx_vol_start="$(extract_status_number "$bench_pid" "voluntary_ctxt_switches")"
    ctx_nonvol_start="$(extract_status_number "$bench_pid" "nonvoluntary_ctxt_switches")"
    syscr_start="$(extract_io_counter "$bench_pid" "syscr")"
    syscw_start="$(extract_io_counter "$bench_pid" "syscw")"
    if [[ -z "$ctx_vol_start" ]]; then ctx_vol_start=0; fi
    if [[ -z "$ctx_nonvol_start" ]]; then ctx_nonvol_start=0; fi
    if [[ -z "$syscr_start" ]]; then syscr_start=0; fi
    if [[ -z "$syscw_start" ]]; then syscw_start=0; fi
    syscr_last="$syscr_start"
    syscw_last="$syscw_start"

    shopt -s nullglob
    while kill -0 "$bench_pid" 2>/dev/null; do
        local rss_now
        rss_now="$(extract_status_number "$bench_pid" "VmRSS")"
        if [[ -n "$rss_now" ]] && [[ "$rss_now" -gt "$rss_peak_kb" ]]; then
            rss_peak_kb="$rss_now"
        fi

        local fd_now=0
        if [[ -d "/proc/${bench_pid}/fd" ]]; then
            local fd_entries=(/proc/${bench_pid}/fd/*)
            fd_now="${#fd_entries[@]}"
        fi
        if [[ "$fd_now" -gt "$fd_peak" ]]; then
            fd_peak="$fd_now"
        fi

        local syscr_now syscw_now
        syscr_now="$(extract_io_counter "$bench_pid" "syscr")"
        syscw_now="$(extract_io_counter "$bench_pid" "syscw")"
        if [[ -n "$syscr_now" ]]; then
            syscr_last="$syscr_now"
        fi
        if [[ -n "$syscw_now" ]]; then
            syscw_last="$syscw_now"
        fi

        sleep "$SAMPLE_INTERVAL_SEC"
    done
    shopt -u nullglob

    end_jiffies="$(extract_proc_jiffies "$bench_pid")"
    ctx_vol_end="$(extract_status_number "$bench_pid" "voluntary_ctxt_switches")"
    ctx_nonvol_end="$(extract_status_number "$bench_pid" "nonvoluntary_ctxt_switches")"
    syscr_end="$(extract_io_counter "$bench_pid" "syscr")"
    syscw_end="$(extract_io_counter "$bench_pid" "syscw")"

    if ! wait "$bench_pid"; then
        printf 'Benchmark process exited with non-zero status (backend=%s connections=%s pps=%s)\n' \
            "$backend" "$connections" "$pps" >&2
        read -r -d '' raw_output < "$tmp_output" || true
        printf '%s\n' "$raw_output" >&2
        rm -f "$tmp_output"
        exit 1
    fi

    end_epoch="$(date +%s.%N)"
    elapsed_sec="$(compute_float "$end_epoch - $start_epoch")"

    if [[ -z "$ctx_vol_end" ]]; then ctx_vol_end="$ctx_vol_start"; fi
    if [[ -z "$ctx_nonvol_end" ]]; then ctx_nonvol_end="$ctx_nonvol_start"; fi
    if [[ -z "$syscr_end" ]]; then syscr_end="$syscr_last"; fi
    if [[ -z "$syscw_end" ]]; then syscw_end="$syscw_last"; fi
    ctx_vol_delta="$((ctx_vol_end - ctx_vol_start))"
    ctx_nonvol_delta="$((ctx_nonvol_end - ctx_nonvol_start))"
    syscr_delta="$((syscr_end - syscr_start))"
    syscw_delta="$((syscw_end - syscw_start))"

    local cpu_avg_pct="0"
    if [[ -n "$start_jiffies" && -n "$end_jiffies" ]] && [[ "$elapsed_sec" != "0.000000" ]]; then
        cpu_avg_pct="$(compute_float "(($end_jiffies - $start_jiffies) / $hz) / $elapsed_sec * 100.0")"
    fi

    local raw_output
    raw_output="$(< "$tmp_output")"
    rm -f "$tmp_output"

    printf '%s\n' "$raw_output"
    printf '%s\n' "__PROC_METRICS__ cpu_avg_pct=${cpu_avg_pct} rss_peak_kb=${rss_peak_kb} fd_peak=${fd_peak} ctx_voluntary=${ctx_vol_delta} ctx_nonvoluntary=${ctx_nonvol_delta} syscall_read_count=${syscr_delta} syscall_write_count=${syscw_delta}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
    --build-dir)
        BUILD_DIR="$2"
        shift 2
        ;;
    --bin)
        BIN_REL="$2"
        shift 2
        ;;
    --workers)
        WORKERS="$2"
        shift 2
        ;;
    --duration)
        DURATION="$2"
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
    --backends)
        BACKENDS_CSV="$2"
        shift 2
        ;;
    --connections)
        CONNECTIONS_CSV="$2"
        shift 2
        ;;
    --pps)
        PPS_CSV="$2"
        shift 2
        ;;
    --repeats)
        REPEATS="$2"
        shift 2
        ;;
    --output)
        OUTPUT="$2"
        shift 2
        ;;
    --sample-interval)
        SAMPLE_INTERVAL_SEC="$2"
        shift 2
        ;;
    --min-pps)
        MIN_PPS="$2"
        shift 2
        ;;
    --max-avg-rtt-us)
        MAX_AVG_RTT_US="$2"
        shift 2
        ;;
    --max-p95-rtt-us)
        MAX_P95_RTT_US="$2"
        shift 2
        ;;
    --max-p99-rtt-us)
        MAX_P99_RTT_US="$2"
        shift 2
        ;;
    --max-max-rtt-us)
        MAX_MAX_RTT_US="$2"
        shift 2
        ;;
    --no-strict)
        STRICT=0
        shift
        ;;
    --skip-build)
        SKIP_BUILD=1
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

if [[ ! -d "$BUILD_DIR" ]]; then
    printf 'Build directory not found: %s\n' "$BUILD_DIR" >&2
    exit 1
fi

if ! is_number "$SAMPLE_INTERVAL_SEC"; then
    printf 'Invalid --sample-interval: %s\n' "$SAMPLE_INTERVAL_SEC" >&2
    exit 1
fi

for threshold in "$MIN_PPS" "$MAX_AVG_RTT_US" "$MAX_P95_RTT_US" "$MAX_P99_RTT_US" "$MAX_MAX_RTT_US"; do
    if [[ -n "$threshold" ]] && ! is_number "$threshold"; then
        printf 'Threshold must be numeric: %s\n' "$threshold" >&2
        exit 1
    fi
done

if [[ "$SKIP_BUILD" -eq 0 ]]; then
    cmake --build "$BUILD_DIR" -j "${JOBS:-8}" --target game_benchmark
fi

BIN_PATH="${BUILD_DIR}/${BIN_REL}"
if [[ ! -x "$BIN_PATH" ]]; then
    printf 'Benchmark binary not found or not executable: %s\n' "$BIN_PATH" >&2
    exit 1
fi

if [[ -z "$OUTPUT" ]]; then
    timestamp="$(date +%Y%m%d_%H%M%S)"
    OUTPUT="${ROOT_DIR}/docs/benchmarks/game_benchmark_matrix_${timestamp}.csv"
fi

OUTPUT_DIR="$(dirname "$OUTPUT")"
mkdir -p "$OUTPUT_DIR"

declare -a BACKENDS
declare -a CONNECTIONS
declare -a PPS_VALUES
split_csv "$BACKENDS_CSV" BACKENDS
split_csv "$CONNECTIONS_CSV" CONNECTIONS
split_csv "$PPS_CSV" PPS_VALUES

printf '%s\n' "run_ts,backend,workers,duration_s,connections,pps_per_conn,completion_batch_size,frame_size,client_threads,sent,received,failed,loss_count,loss_ratio,packets_per_second,avg_rtt_us,p50_rtt_us,p95_rtt_us,p99_rtt_us,max_rtt_us,cpu_avg_pct,rss_peak_kb,fd_peak,ctx_voluntary,ctx_nonvoluntary,syscall_read_count,syscall_write_count" > "$OUTPUT"

for backend in "${BACKENDS[@]}"; do
    for connections in "${CONNECTIONS[@]}"; do
        effective_client_threads="$CLIENT_THREADS"
        if [[ "$effective_client_threads" -gt "$connections" ]]; then
            effective_client_threads="$connections"
        fi
        for pps in "${PPS_VALUES[@]}"; do
            run_index=1
            while [[ "$run_index" -le "$REPEATS" ]]; do
                printf 'Running backend=%s connections=%s pps=%s repeat=%s/%s\n' \
                    "$backend" "$connections" "$pps" "$run_index" "$REPEATS"

                output="$(run_one_case "$backend" "$connections" "$pps" "$effective_client_threads")"

                header_line=""
                result_line=""
                proc_line=""
                while IFS= read -r line; do
                    if [[ "$line" == game\ server\ benchmark* ]]; then
                        header_line="$line"
                    fi
                    if [[ "$line" == sent=* ]]; then
                        result_line="$line"
                    fi
                    if [[ "$line" == __PROC_METRICS__* ]]; then
                        proc_line="$line"
                    fi
                done <<< "$output"

                if [[ -z "$header_line" || -z "$result_line" ]]; then
                    printf 'Failed to parse benchmark output for backend=%s connections=%s pps=%s\n' \
                        "$backend" "$connections" "$pps" >&2
                    printf '%s\n' "$output" >&2
                    exit 1
                fi

                parsed_backend="$(extract_header_value "$header_line" "backend")"
                parsed_workers="$(extract_header_value "$header_line" "workers")"
                parsed_duration="$(extract_header_value "$header_line" "duration_s")"
                parsed_connections="$(extract_header_value "$header_line" "connections")"
                parsed_pps="$(extract_header_value "$header_line" "pps_per_connection")"
                parsed_completion="$(extract_header_value "$header_line" "completion_batch_size")"
                parsed_frame_size="$(extract_header_value "$header_line" "frame_size")"
                parsed_client_threads="$(extract_header_value "$header_line" "client_threads")"

                sent="$(extract_result_value "$result_line" "sent")"
                received="$(extract_result_value "$result_line" "received")"
                failed="$(extract_result_value "$result_line" "failed")"
                packets_per_second="$(extract_result_value "$result_line" "packets_per_second")"
                avg_rtt_us="$(extract_result_value "$result_line" "avg_rtt_us")"
                p50_rtt_us="$(extract_result_value "$result_line" "p50_rtt_us")"
                p95_rtt_us="$(extract_result_value "$result_line" "p95_rtt_us")"
                p99_rtt_us="$(extract_result_value "$result_line" "p99_rtt_us")"
                max_rtt_us="$(extract_result_value "$result_line" "max_rtt_us")"
                cpu_avg_pct="$(extract_result_value "$proc_line" "cpu_avg_pct")"
                rss_peak_kb="$(extract_result_value "$proc_line" "rss_peak_kb")"
                fd_peak="$(extract_result_value "$proc_line" "fd_peak")"
                ctx_voluntary="$(extract_result_value "$proc_line" "ctx_voluntary")"
                ctx_nonvoluntary="$(extract_result_value "$proc_line" "ctx_nonvoluntary")"
                syscall_read_count="$(extract_result_value "$proc_line" "syscall_read_count")"
                syscall_write_count="$(extract_result_value "$proc_line" "syscall_write_count")"

                require_non_empty "$parsed_backend" "backend"
                require_non_empty "$parsed_workers" "workers"
                require_non_empty "$parsed_duration" "duration_s"
                require_non_empty "$parsed_connections" "connections"
                require_non_empty "$parsed_pps" "pps_per_connection"
                require_non_empty "$parsed_completion" "completion_batch_size"
                require_non_empty "$parsed_frame_size" "frame_size"
                require_non_empty "$parsed_client_threads" "client_threads"
                require_non_empty "$sent" "sent"
                require_non_empty "$received" "received"
                require_non_empty "$failed" "failed"
                require_non_empty "$packets_per_second" "packets_per_second"
                require_non_empty "$avg_rtt_us" "avg_rtt_us"
                require_non_empty "$p50_rtt_us" "p50_rtt_us"
                require_non_empty "$p95_rtt_us" "p95_rtt_us"
                require_non_empty "$p99_rtt_us" "p99_rtt_us"
                require_non_empty "$max_rtt_us" "max_rtt_us"
                require_non_empty "$cpu_avg_pct" "cpu_avg_pct"
                require_non_empty "$rss_peak_kb" "rss_peak_kb"
                require_non_empty "$fd_peak" "fd_peak"
                require_non_empty "$ctx_voluntary" "ctx_voluntary"
                require_non_empty "$ctx_nonvoluntary" "ctx_nonvoluntary"
                require_non_empty "$syscall_read_count" "syscall_read_count"
                require_non_empty "$syscall_write_count" "syscall_write_count"

                loss_count="$((sent - received))"
                if [[ "$sent" -eq 0 ]]; then
                    loss_ratio="0"
                else
                    loss_ratio="$(compute_float "$loss_count / $sent")"
                fi

                if [[ "$STRICT" -eq 1 ]]; then
                    if [[ "$failed" != "0" ]]; then
                        printf 'Strict check failed: failed=%s (backend=%s connections=%s pps=%s)\n' \
                            "$failed" "$parsed_backend" "$parsed_connections" "$parsed_pps" >&2
                        exit 1
                    fi
                    if [[ "$sent" != "$received" ]]; then
                        printf 'Strict check failed: sent=%s received=%s (backend=%s connections=%s pps=%s)\n' \
                            "$sent" "$received" "$parsed_backend" "$parsed_connections" "$parsed_pps" >&2
                        exit 1
                    fi
                    if [[ -n "$MIN_PPS" ]] && ! awk "BEGIN { exit !($packets_per_second >= $MIN_PPS) }"; then
                        printf 'Strict check failed: packets_per_second=%s < min=%s (backend=%s connections=%s pps=%s)\n' \
                            "$packets_per_second" "$MIN_PPS" "$parsed_backend" "$parsed_connections" "$parsed_pps" >&2
                        exit 1
                    fi
                    if [[ -n "$MAX_AVG_RTT_US" ]] && ! awk "BEGIN { exit !($avg_rtt_us <= $MAX_AVG_RTT_US) }"; then
                        printf 'Strict check failed: avg_rtt_us=%s > max=%s (backend=%s connections=%s pps=%s)\n' \
                            "$avg_rtt_us" "$MAX_AVG_RTT_US" "$parsed_backend" "$parsed_connections" "$parsed_pps" >&2
                        exit 1
                    fi
                    if [[ -n "$MAX_P95_RTT_US" ]] && ! awk "BEGIN { exit !($p95_rtt_us <= $MAX_P95_RTT_US) }"; then
                        printf 'Strict check failed: p95_rtt_us=%s > max=%s (backend=%s connections=%s pps=%s)\n' \
                            "$p95_rtt_us" "$MAX_P95_RTT_US" "$parsed_backend" "$parsed_connections" "$parsed_pps" >&2
                        exit 1
                    fi
                    if [[ -n "$MAX_P99_RTT_US" ]] && ! awk "BEGIN { exit !($p99_rtt_us <= $MAX_P99_RTT_US) }"; then
                        printf 'Strict check failed: p99_rtt_us=%s > max=%s (backend=%s connections=%s pps=%s)\n' \
                            "$p99_rtt_us" "$MAX_P99_RTT_US" "$parsed_backend" "$parsed_connections" "$parsed_pps" >&2
                        exit 1
                    fi
                    if [[ -n "$MAX_MAX_RTT_US" ]] && ! awk "BEGIN { exit !($max_rtt_us <= $MAX_MAX_RTT_US) }"; then
                        printf 'Strict check failed: max_rtt_us=%s > max=%s (backend=%s connections=%s pps=%s)\n' \
                            "$max_rtt_us" "$MAX_MAX_RTT_US" "$parsed_backend" "$parsed_connections" "$parsed_pps" >&2
                        exit 1
                    fi
                fi

                run_ts="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
                printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
                    "$run_ts" \
                    "$parsed_backend" \
                    "$parsed_workers" \
                    "$parsed_duration" \
                    "$parsed_connections" \
                    "$parsed_pps" \
                    "$parsed_completion" \
                    "$parsed_frame_size" \
                    "$parsed_client_threads" \
                    "$sent" \
                    "$received" \
                    "$failed" \
                    "$loss_count" \
                    "$loss_ratio" \
                    "$packets_per_second" \
                    "$avg_rtt_us" \
                    "$p50_rtt_us" \
                    "$p95_rtt_us" \
                    "$p99_rtt_us" \
                    "$max_rtt_us" \
                    "$cpu_avg_pct" \
                    "$rss_peak_kb" \
                    "$fd_peak" \
                    "$ctx_voluntary" \
                    "$ctx_nonvoluntary" \
                    "$syscall_read_count" \
                    "$syscall_write_count" >> "$OUTPUT"

                run_index=$((run_index + 1))
            done
        done
    done
done

printf 'Done. Wrote CSV: %s\n' "$OUTPUT"
