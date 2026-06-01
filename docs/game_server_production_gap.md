# Game Server Production Gap

This document tracks what is still needed before the networking stack should be treated as a production game-server base.

## Current Foundation

- TCP async listener supports throughput and affinity scheduling modes.
- IOCP completion batching is configurable.
- Generic sharded listener exists for epoll/kqueue/poller style backends.
- Game long-connection benchmark exists for small-packet RTT testing (`test/benchmark/game_benchmark.cpp`).
- Business switch-case coroutine utility exists as an independent library target.
- Listener admission limits, input/output buffer limits, and listener metrics have first-pass Core support.
- The business coroutine library has typed errors, cancellation/timeout helpers, child-flow waiting, compensation hooks, and trace hooks.

## Execution Tracker

Use this section as the active working board. Keep each item with status, owner, and evidence.

Status legend:
- `todo`: not started
- `in_progress`: currently running
- `done`: accepted
- `blocked`: waiting for dependency

### 1. Linux Validation

- [done] Build and run Release on Linux.
  - owner: TBD
  - evidence: `game_benchmark` and other benchmark binaries compile and run on Linux.
- [done] Validate epoll and epoll_affinity modes with current defaults.
  - owner: TBD
  - evidence (2026-06-01):
    - epoll: `packets_per_second=10244.5`, `avg_rtt_us=173.831`, `p99<=1000`, `failed=0`
    - epoll_affinity: `packets_per_second=10242`, `avg_rtt_us=104.533`, `p99<=500`, `failed=0`
- [todo] Benchmark epoll throughput vs epoll_affinity with `reuse_port`.
  - owner: TBD
  - acceptance: same test matrix and environment for both modes.
- [todo] Measure 1k, 10k, and 50k long connections.
  - owner: TBD
  - acceptance: all runs complete without connection leak and with stable RTT percentiles.
- [todo] Measure 20/50/100 packets per second per connection.
  - owner: TBD
  - acceptance: throughput and latency curves are recorded per mode.
- [todo] Track p50/p95/p99/p999 RTT, CPU, RSS, fd count, context switches, and syscalls.
  - owner: TBD
  - acceptance: metrics exported to one CSV per run.
  - progress: `scripts/bench_game_matrix.sh` now exports RTT, loss, CPU (avg), RSS peak, fd peak, context-switch deltas, and syscall counters.
- [todo] Run long soak tests for 12 to 24 hours.
  - owner: TBD
  - acceptance: no crash, no leak trend, no unbounded tail-latency drift.
  - progress: `scripts/bench_game_soak.sh` added for segmented strict soak runs with resume support.

### 2. Connection Lifecycle

- [todo] Define close reasons for normal close, peer close, timeout, protocol error, backpressure, admission reject, and internal error.
- [todo] Make half-close behavior explicit for every stream backend.
- [todo] Ensure timers and pending coroutine waiters are released on close.
- [todo] Ensure connection handlers cannot keep connections alive accidentally after close.
- [done] Listener guarded handlers release admission counters on normal completion and coroutine unwinding.
- [todo] Add soak tests for peer reset, half-close, slow reads, slow writes, and close while flush is pending.

### 3. Backpressure

- [done] `ListenOptions::max_input_buffer_bytes` is applied to accepted connections.
- [done] `ListenOptions::max_output_buffer_bytes` is enforced by TCP/IOCP output buffering.
- [done] Coroutine-facing append paths close the connection when output policy is exceeded.
- [todo] Add shard-level queued callback/task pressure metrics.
- [todo] Add broadcast-safe write APIs that can skip or close overloaded clients.

### 4. Shard Model

- [todo] Define connection-to-shard policy.
- [todo] Define player/room/map-to-shard routing.
- [todo] Add explicit cross-shard message queues.
- [todo] Add shard-local timers and tick delay metrics.
- [todo] Avoid moving high-frequency room state across shards.

### 5. Protocol Layer

- [todo] Define packet frame format with magic/version/length/opcode/sequence.
- [todo] Enforce packet size limits before allocation-heavy parsing.
- [todo] Add heartbeat, idle timeout, replay protection, and optional encryption.
- [todo] Add fuzz tests for malformed frames.

### 6. Business Coroutine Library

- [done] Keep this outside Core as `GameCoroutine`.
- [done] Add child-flow composition.
- [done] Add cancellation and timeout helpers.
- [done] Add typed error codes.
- [done] Add rollback/compensation hooks.
- [done] Add tracing hooks for inventory/reward/payment flows.
- [done] Add pending coroutine pool for cross-process request/response wakeups.
  - evidence:
    - `libs/game_coroutine/include/game_coroutine/coroutine_pool.h`
    - supports id-based wake (`complete/failed/canceled`), timeout wake, typed `std::any` context, batch cancel, and stats snapshot.
- [done] Add reusable pending RPC runtime wrapper.
  - evidence:
    - `libs/game_coroutine/include/game_coroutine/pending_rpc_service.h`
    - supports begin request, per-request context, route/owner/predicate cancel, timeout tick, pending snapshot/restore, and stats access.
- [done] Add coverage tests for pool and service runtime behavior.
  - evidence:
    - `test/core/test_coroutine_pool.cpp`
    - `test/core/test_coroutine_pool_service.cpp`
    - `test/core/test_pending_rpc_service.cpp`

### 7. Observability

- [done] Expose listener active, accepted, rejected, completed-handler, and backpressure-close counters.
- [todo] Expose read/write bytes and pending output bytes.
- [todo] Expose per-loop pending callbacks and pending coroutine counts.
- [todo] Expose timer counts and tick delay.
- [todo] Add Prometheus or structured metrics output in server applications.

### 8. Security And Abuse Protection

- [done] Enforce max listener connections globally and per IP.
- [done] Enforce per-connection packet/input and output buffer limits.
- [todo] Add login-before-heavy-work gates.
- [todo] Add pre-auth packet rate limit.
- [todo] Add shard overload protection.
- [todo] Add blacklist/graylist hooks.

### 9. Deployment

- [todo] Provide systemd units.
- [todo] Document `ulimit`, `somaxconn`, backlog, ephemeral port, and TCP keepalive settings.
- [todo] Support graceful shutdown and drain.
- [todo] Support core dump/crash dump collection.
- [todo] Provide CPU affinity and NUMA guidance.

## Runbook Commands

- Build benchmark target:
  - `cmake --build build -j 8 --target game_benchmark`
- Run default mode (Linux default is epoll):
  - `./build/test/benchmark/game_benchmark`
- Run epoll affinity mode:
  - `./build/test/benchmark/game_benchmark 4 10 512 20 1 32 16 epoll_affinity`
- Run strict matrix benchmark (example):
  - `./scripts/bench_game_matrix.sh --connections 1000,10000,50000 --pps 20,50,100 --backends epoll,epoll_affinity --repeats 3 --duration 30 --output docs/benchmarks/game_benchmark_matrix_full_strict.csv --min-pps 9000 --max-p95-rtt-us 2000 --max-p99-rtt-us 5000 --max-max-rtt-us 300000`
- Generate markdown summary from CSV:
  - `./scripts/summarize_game_matrix.sh --input docs/benchmarks/game_benchmark_matrix_full_strict.csv --output docs/benchmarks/game_benchmark_matrix_full_strict_summary.md`
- Run segmented soak benchmark with strict thresholds (example 12h):
  - `./scripts/bench_game_soak.sh --backend epoll_affinity --connections 10000 --pps 50 --total-duration 43200 --segment-duration 300 --output docs/benchmarks/game_benchmark_soak_12h.csv --min-pps 9000 --max-p95-rtt-us 3000 --max-p99-rtt-us 6000 --max-max-rtt-us 300000`

## Immediate Priority

The first production-hardening batch has Core-level first-pass support, and the next milestone is Linux proof at scale:

1. Complete 1k/10k/50k connection and 20/50/100 pps matrix on epoll and epoll_affinity.
2. Add runtime/system metrics capture to every benchmark run.
3. Execute at least one 12-hour soak and one 24-hour soak.
4. Close remaining lifecycle and pre-auth rate-limit gaps.
