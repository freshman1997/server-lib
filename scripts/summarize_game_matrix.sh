#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INPUT=""
OUTPUT=""

usage() {
    printf '%s\n' "Usage: scripts/summarize_game_matrix.sh --input <csv> [--output <md>]"
    printf '%s\n' ""
    printf '%s\n' "Options:"
    printf '%s\n' "  --input <path>    Input CSV from bench_game_matrix.sh"
    printf '%s\n' "  --output <path>   Output markdown path (default: docs/benchmarks/<input_basename>_summary.md)"
    printf '%s\n' "  -h, --help        Show this message"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
    --input)
        INPUT="$2"
        shift 2
        ;;
    --output)
        OUTPUT="$2"
        shift 2
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

if [[ -z "$INPUT" ]]; then
    printf 'Missing required option: --input\n' >&2
    usage >&2
    exit 1
fi

if [[ ! -f "$INPUT" ]]; then
    printf 'Input file not found: %s\n' "$INPUT" >&2
    exit 1
fi

if [[ -z "$OUTPUT" ]]; then
    input_name="$(basename "$INPUT")"
    input_stem="${input_name%.csv}"
    OUTPUT="${ROOT_DIR}/docs/benchmarks/${input_stem}_summary.md"
fi

output_dir="$(dirname "$OUTPUT")"
mkdir -p "$output_dir"

awk -F',' '
NR == 1 {
    next
}
NR > 1 {
    key = $2 "|" $5 "|" $6
    runs[key]++

    pps = $15 + 0
    avg = $16 + 0
    p95 = $18 + 0
    p99 = $19 + 0
    maxrtt = $20 + 0
    failed = $12 + 0
    loss = $13 + 0
    sys_read = $26 + 0
    sys_write = $27 + 0

    pps_sum[key] += pps
    avg_sum[key] += avg
    failed_sum[key] += failed
    loss_sum[key] += loss
    sys_read_sum[key] += sys_read
    sys_write_sum[key] += sys_write

    if (!(key in pps_min) || pps < pps_min[key]) pps_min[key] = pps
    if (!(key in pps_max) || pps > pps_max[key]) pps_max[key] = pps
    if (!(key in p95_max) || p95 > p95_max[key]) p95_max[key] = p95
    if (!(key in p99_max) || p99 > p99_max[key]) p99_max[key] = p99
    if (!(key in maxrtt_max) || maxrtt > maxrtt_max[key]) maxrtt_max[key] = maxrtt

    if (failed == 0 && loss == 0) {
        pass_runs[key]++
    }

    total_runs++
    total_failed += failed
    total_loss += loss
    total_pps_sum += pps
    total_sys_read += sys_read
    total_sys_write += sys_write
}
END {
    if (total_runs == 0) {
        print "# Game Benchmark Summary"
        print ""
        print "No data rows found."
        exit 0
    }

    print "# Game Benchmark Summary"
    print ""
    print "- Total runs: " total_runs
    print "- Total failed packets: " total_failed
    print "- Total loss packets: " total_loss
    printf "- Mean packets_per_second: %.2f\n", (total_pps_sum / total_runs)
    print "- Total syscall_read_count: " total_sys_read
    print "- Total syscall_write_count: " total_sys_write
    print ""
    print "| backend | connections | pps_per_conn | runs | pass_runs | pass_rate | pps_avg | pps_min | pps_max | avg_rtt_us_mean | p95_rtt_us_max | p99_rtt_us_max | max_rtt_us_max | failed_sum | loss_sum | syscall_read_sum | syscall_write_sum |"
    print "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"

    for (key in runs) {
        split(key, parts, "|")
        backend = parts[1]
        connections = parts[2]
        pps_per_conn = parts[3]

        r = runs[key]
        p = pass_runs[key] + 0
        fr = (p * 100.0) / r

        printf "| %s | %s | %s | %d | %d | %.2f%% | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f | %d | %d | %d | %d |\n", \
            backend, connections, pps_per_conn, r, p, fr, \
            (pps_sum[key] / r), pps_min[key], pps_max[key], \
            (avg_sum[key] / r), p95_max[key], p99_max[key], maxrtt_max[key], \
            failed_sum[key], loss_sum[key], sys_read_sum[key], sys_write_sum[key]
    }
}
' "$INPUT" > "$OUTPUT"

printf 'Done. Wrote summary: %s\n' "$OUTPUT"
