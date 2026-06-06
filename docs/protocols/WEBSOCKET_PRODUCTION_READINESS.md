# WebSocket Production Readiness

This document tracks the WebSocket hardening work needed before the standalone
WebSocket service or WebSocket proxy path is used for public production traffic.

## Current Verdict

The implementation has a usable baseline: HTTP upgrade handshake, frame
packing/unpacking, text/binary frames, close, ping/pong, fragmentation, a 1 MiB
message cap, service lifecycle integration, and a WebSocket-aware HTTP proxy
handoff.

It is not production-ready yet. It is acceptable for internal experiments or
controlled clients, but public service should wait until the items below are
complete and verified.

## Phase 1 - RFC 6455 Compliance

- [x] Accept tokenized `Connection` headers such as `keep-alive, Upgrade`.
- [x] Validate `Sec-WebSocket-Key` as base64 that decodes to exactly 16 bytes.
- [x] Force client-to-server frames to be masked by default.
- [x] Reject masked server-to-client frames.
- [x] Reject unmasked client-to-server frames.
- [x] Reject RSV bits unless an extension has explicitly enabled them.
- [x] Reject unknown opcodes.
- [x] Enforce control-frame rules: final frame only and payload <= 125 bytes.
- [ ] Validate close payload length and close code ranges.
- [ ] Validate text messages as UTF-8, at least before exposing them as text.
- [x] Add focused unit tests for the completed cases above.

## Phase 2 - Connection Lifecycle

- [ ] Replace handler-facing stack-owned `WebSocketConnection *` usage with a
      session object that has clear ownership and async-safe send semantics.
- [ ] Add a per-connection outbound queue with configurable byte/message limits.
- [ ] Surface backpressure to handlers instead of writing directly without
      limits.
- [ ] Make close handshake stateful: close-sent, close-received, deadline,
      forced close.
- [ ] Ensure shutdown cancels timers and outstanding async operations cleanly.

## Phase 3 - Timeouts And Heartbeat

- [ ] Add handshake timeout.
- [ ] Add read idle timeout.
- [ ] Rework ping/pong heartbeat around a pong deadline.
- [ ] Initialize activity timestamps on connect.
- [ ] Track last read, last write, last ping, and last pong separately.

## Phase 4 - Security And Policy Hooks

- [ ] Add Origin validation hook.
- [ ] Add authentication/authorization hook before upgrade is accepted.
- [ ] Add configurable maximum message size and maximum fragmented message size.
- [ ] Add per-IP connection limits.
- [ ] Add per-IP handshake/message rate limiting.
- [ ] Make TLS certificate/key paths configurable instead of hard-coded.

## Phase 5 - Proxy Path

- [ ] Use the same RFC-compliant handshake checks for proxy handoff.
- [ ] Add backend handshake timeout, not only connect timeout.
- [ ] Preserve or intentionally filter relevant upgrade headers.
- [ ] Add proxy metrics for active WebSocket tunnels, tunnel bytes, close reason,
      backend handshake failures, and protocol failures.
- [ ] Add e2e proxy tests with masked browser-like clients.

## Phase 6 - Verification Gate

- [ ] Register WebSocket tests with CTest.
- [ ] Run Autobahn Testsuite and record the report.
- [ ] Add browser compatibility smoke tests.
- [ ] Add malformed-frame tests.
- [ ] Add slowloris/slow-handshake tests.
- [ ] Add concurrent connection and message throughput benchmarks.
- [ ] Add soak test: at least 24 hours with heartbeat, reconnect, and mixed
      payload sizes.

## Rollout Criteria

Production rollout is allowed only after Phase 1 through Phase 4 are complete,
proxy users complete Phase 5, and Phase 6 has a recorded passing report.
