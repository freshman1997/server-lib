# Game Server Production Gap

This document tracks what is still needed before the networking stack should be treated as a production game-server base.

## Current Foundation

- TCP async listener supports throughput and affinity scheduling modes.
- IOCP completion batching is configurable.
- Generic sharded listener exists for poller/epoll/kqueue style backends.
- Game long-connection benchmark exists for small-packet RTT testing.
- Business switch-case coroutine utility exists as an independent library target.
- Listener admission limits, input/output buffer limits, and listener metrics have first-pass Core support.
- The business coroutine library has typed errors, cancellation/timeout helpers, child-flow waiting, compensation hooks, and trace hooks.

## Remaining Production Work

### 1. Linux Validation

- Build and run Release on Linux.
- Benchmark epoll throughput vs epoll affinity with `reuse_port`.
- Measure 1k, 10k, and 50k long connections.
- Measure 20/50/100 packets per second per connection.
- Track p50/p95/p99/p999 RTT, CPU, RSS, fd count, context switches, and syscalls.
- Run long soak tests for 12 to 24 hours.

### 2. Connection Lifecycle

- Define close reasons for normal close, peer close, timeout, protocol error, backpressure, admission reject, and internal error.
- Make half-close behavior explicit for every stream backend.
- Ensure timers and pending coroutine waiters are released on close.
- Ensure connection handlers cannot keep connections alive accidentally after close.
- Done first-pass: listener guarded handlers now release admission counters on normal completion and coroutine unwinding.
- Add soak tests for peer reset, half-close, slow reads, slow writes, and close while flush is pending.

### 3. Backpressure

- Done first-pass: `ListenOptions::max_input_buffer_bytes` is applied to accepted connections.
- Done first-pass: `ListenOptions::max_output_buffer_bytes` is enforced by TCP/IOCP output buffering.
- Done first-pass: coroutine-facing append paths close the connection when output policy is exceeded.
- Add shard-level queued callback/task pressure metrics.
- Add broadcast-safe write APIs that can skip or close overloaded clients.

### 4. Shard Model

- Define connection-to-shard policy.
- Define player/room/map-to-shard routing.
- Add explicit cross-shard message queues.
- Add shard-local timers and tick delay metrics.
- Avoid moving high-frequency room state across shards.

### 5. Protocol Layer

- Define packet frame format with magic/version/length/opcode/sequence.
- Enforce packet size limits before allocation-heavy parsing.
- Add heartbeat, idle timeout, replay protection, and optional encryption.
- Add fuzz tests for malformed frames.

### 6. Business Coroutine Library

- Done first-pass: keep this outside Core as `GameCoroutine`.
- Done first-pass: add child-flow composition.
- Done first-pass: add cancellation and timeout helpers.
- Done first-pass: add typed error codes.
- Done first-pass: add rollback/compensation hooks.
- Done first-pass: add tracing hooks for inventory/reward/payment flows.

### 7. Observability

- Done first-pass: expose listener active, accepted, rejected, completed-handler, and backpressure-close counters.
- Expose read/write bytes and pending output bytes.
- Expose per-loop pending callbacks and pending coroutine counts.
- Expose timer counts and tick delay.
- Add Prometheus or structured metrics output in server applications.

### 8. Security And Abuse Protection

- Done first-pass: enforce max listener connections globally and per IP.
- Done first-pass: enforce per-connection packet/input and output buffer limits.
- Add login-before-heavy-work gates.
- Add pre-auth packet rate limit.
- Add shard overload protection.
- Add blacklist/graylist hooks.

### 9. Deployment

- Provide systemd units.
- Document `ulimit`, `somaxconn`, backlog, ephemeral port, and TCP keepalive settings.
- Support graceful shutdown and drain.
- Support core dump/crash dump collection.
- Provide CPU affinity and NUMA guidance.

## Immediate Priority

The first production-hardening batch now has Core-level first-pass support:

1. Connection lifecycle counters are present at listener-handler scope; explicit close reason plumbing remains.
2. Input/output backpressure limits are implemented for accepted connections and direct append paths.
3. Listener metrics snapshots are implemented; runtime/shard queue metrics remain.
4. Admission and packet-size security limits are implemented at listener level; pre-auth rate limits remain.
