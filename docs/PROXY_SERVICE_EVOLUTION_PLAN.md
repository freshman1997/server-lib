# Proxy Service Evolution Plan

## Background

Current [test/proxy/proxy_tool.cpp](/home/yuan/codes/test/webserver/test/proxy/proxy_tool.cpp) is a test-oriented utility:

- It mixes multiple roles: SOCKS5 server launcher, HTTP CONNECT proxy, UDP echo, UDP probe.
- It uses native blocking socket I/O and per-connection threads for the HTTP CONNECT path.
- It is good for quick protocol verification, but it is not shaped like a long-running production service.

The goal of this document is to describe how to evolve it into a formal long-running proxy service built on top of the existing project runtime, network abstractions, and service hosting model.

## Goals

- Rebuild proxy capability as a formal service rather than a test tool.
- Reuse existing runtime abstractions:
  - `NetworkRuntime`
  - `Connection` / `ConnectionHandler`
  - `Socket` / acceptor abstractions
  - service hosting under `server/services`
- Support long-running operation with:
  - bounded resource usage
  - connection lifecycle management
  - idle timeout cleanup
  - graceful shutdown
  - structured logging and metrics hooks
- Preserve the ability to support:
  - HTTP CONNECT tunneling
  - SOCKS5 proxying
  - optional UDP associate / relay

## Non-Goals

- Do not keep `proxy_tool` as the main service implementation.
- Do not force all current probe helpers into the formal service layer.
- Do not implement full forward HTTP proxying for plain HTTP methods in phase 1 unless explicitly needed.
- Do not start with connection pooling for CONNECT tunnels; correctness and lifecycle management come first.

## Current Constraints

### Why `proxy_tool` exists in its current form

The current implementation is optimized for testing:

- fast to write
- easy to debug at the packet level
- no dependency on event-loop ownership rules
- convenient for standalone manual verification

This is why it currently uses direct `accept` / `select` / `recv` / `send` style logic rather than the project runtime.

### Why that model is not enough for formal service mode

For long-running service mode, the current model creates problems:

- one detached thread per tunnel is expensive
- connection lifetime is not centrally managed
- no built-in backpressure model
- shutdown and cleanup are ad hoc
- UDP relay session state is not integrated with runtime timers
- service registration and operational config are outside `server/services`

## Existing Building Blocks In Repo

The repo already has useful foundations:

- [server/services/src/http_service.cpp](/home/yuan/codes/test/webserver/server/services/src/http_service.cpp)
  - shows the formal service hosting pattern
- [protocol/http/src/proxy.cpp](/home/yuan/codes/test/webserver/protocol/http/src/proxy.cpp)
  - contains existing proxy-related logic and upstream connection management ideas
- [protocol/socks5/src/socks5_server.cpp](/home/yuan/codes/test/webserver/protocol/socks5/src/socks5_server.cpp)
  - already uses runtime-oriented connection handling and includes UDP associate support
- [docs/NETWORK_RUNTIME_REFACTOR_PLAN.md](/home/yuan/codes/test/webserver/docs/NETWORK_RUNTIME_REFACTOR_PLAN.md)
  - good reference for aligning with runtime direction

This means the recommended path is not “wrap `proxy_tool` harder”; it is “extract service-worthy behavior into formal protocol/service modules”.

## Recommended Target Architecture

### 1. Split tooling from service

Keep `proxy_tool` only for:

- smoke testing
- manual protocol probing
- local integration verification

Move formal proxy serving into a dedicated service/module pair.

Recommended new components:

- `protocol/proxy/`
  - protocol/runtime-facing proxy implementation
- `server/services/src/proxy_service.cpp`
  - formal service wrapper
- `server/services/include/proxy_service.h`
  - service interface

Optional alternative:

- place HTTP CONNECT-specific logic under `protocol/http/`
- place SOCKS5-specific logic under `protocol/socks5/`
- add a higher-level `ProxyService` that composes them

### 2. Service decomposition

Recommended decomposition:

- `ProxyService`
  - service lifecycle
  - config loading
  - startup/shutdown
  - registration with app/service host
- `HttpConnectProxyServer`
  - accept inbound HTTP proxy connections
  - parse CONNECT requests
  - create tunnel sessions
- `HttpConnectTunnelSession`
  - owns client/upstream pair
  - drives bidirectional forwarding
  - manages close/error/timeout behavior
- `Socks5ProxyServiceAdapter`
  - wraps or composes existing `Socks5Server`
  - exposes formal lifecycle and config
- `UdpRelayManager`
  - manages UDP associate state
  - timeout cleanup
  - relay endpoint mapping
  - per-session metrics

### 3. Session model

A formal tunnel should be a stateful object instead of a detached thread.

Suggested tunnel state:

- session id
- client connection
- upstream connection
- creation time
- last activity time
- bytes client to upstream
- bytes upstream to client
- closing flag
- idle timeout
- route / target metadata

Suggested states:

- `accepted`
- `connecting_upstream`
- `established`
- `half_closed_client`
- `half_closed_upstream`
- `closing`
- `closed`

### 4. I/O model

Recommended approach:

- use runtime-managed `Connection` objects
- register handlers on both client and upstream sides
- forward readable buffers between the two endpoints
- close both sides on unrecoverable error
- update last-activity timestamp on every successful read/write path

Avoid in formal service mode:

- per-session blocking `select`
- detached unmanaged threads
- raw fd ownership spread across helper functions

## Current Implementation Progress

The repo now has a service-oriented proxy baseline in place:

- `ProxyService` under `server/services`
- `proxy_server` as the formal launcher
- HTTP `CONNECT` support
- basic plain HTTP forward proxy support
- optional Basic proxy auth
- target ACL via allow/deny rules
- custom events and counters
- explicit session state transitions and state-change events
- periodic state-driven session sweeps for request/connect phase timeout control
- tracked worker sessions with graceful shutdown
- configurable `drain_timeout_ms` to bound stop-time waiting

This is still not the final runtime/session architecture described above, but it is already beyond the original test-tool shape and is suitable for continued hardening.

## Phase Plan

## Phase 1: Formalize HTTP CONNECT as a service

Scope:

- support only HTTP CONNECT
- no plain HTTP method forwarding
- no connection pooling
- basic config, timeout, logging, graceful stop

Deliverables:

- `ProxyService` registered under `server/services`
- listener hosted on shared runtime when available
- tunnel session object for each CONNECT request
- idle timeout and max connection count
- basic counters
- graceful drain timeout on shutdown
- stable session ids for logs and custom events

Why first:

- CONNECT is the cleanest path from current code to service architecture
- tunneling maps well to connection pair abstractions
- it gives immediate operational value

## Phase 2: Integrate SOCKS5 as formal hosted service

Scope:

- either wrap existing `Socks5Server`
- or expose it through a unified `ProxyService` config surface

Deliverables:

- formal service startup/shutdown
- shared config format
- shared metrics and operational controls

Recommended stance:

- reuse [protocol/socks5/src/socks5_server.cpp](/home/yuan/codes/test/webserver/protocol/socks5/src/socks5_server.cpp) rather than reimplementing SOCKS5 logic

## Phase 3: UDP associate service hardening

Scope:

- session-bound UDP relay
- relay socket lifecycle
- timeout cleanup via timers/runtime
- per-client relay limits

Key requirements:

- UDP association table keyed by session/client
- inactivity timeout
- max datagram size
- target address validation
- cleanup when TCP control channel closes

This phase should happen after the TCP CONNECT path is service-stable.

## Detailed Design Notes

### A. Listener hosting

Follow the same hosting pattern used by formal services:

- init with shared runtime when present
- otherwise create isolated runtime
- expose `init()`, `start()`, `stop()`

This aligns with [server/services/src/http_service.cpp](/home/yuan/codes/test/webserver/server/services/src/http_service.cpp).

### B. CONNECT request handling

Recommended flow:

1. accept client connection
2. parse request head incrementally
3. validate method is `CONNECT`
4. parse target host and port
5. apply ACL / route / policy checks
6. asynchronously connect upstream
7. send `200 Connection Established`
8. switch session into tunnel forwarding mode

Failure responses:

- `400` malformed request
- `405` unsupported method
- `403` forbidden target by policy
- `502` upstream connect failure
- `504` upstream connect timeout

### C. Forwarding strategy

Recommended forwarding strategy:

- use two handlers bound by one session object
- on client read:
  - move readable data to upstream output buffer
  - request flush
- on upstream read:
  - move readable data to client output buffer
  - request flush
- on either close/error:
  - transition session to closing
  - close peer if still alive

Important:

- do not assume both sides always close symmetrically
- support half-close semantics if current connection abstractions allow it
- otherwise use close-both for the first implementation

### D. Buffering and backpressure

Formal service mode should explicitly define limits:

- max header size
- max in-flight tunnel buffer per session
- max total tunnel memory
- per-service max concurrent tunnels

If current connection layer does not expose rich backpressure control yet, start with:

- bounded pending bytes per session
- drop/close session on overflow
- log the reason clearly

### E. Timeouts

Recommended timeout set:

- upstream connect timeout
- client header read timeout
- tunnel idle timeout
- UDP association idle timeout

Timeout handling should go through runtime timers rather than thread sleeping.

### F. Graceful shutdown

Service stop should:

1. stop accepting new sessions
2. mark service as draining
3. close idle sessions immediately
4. allow active sessions a short grace period
5. force close remaining sessions after deadline

This is important if the service is later used in production or test automation environments.

## Configuration Proposal

Suggested config model:

```json
{
  "proxy": {
    "enabled": true,
    "listen_host": "0.0.0.0",
    "listen_port": 3128,
    "enable_http_connect": true,
    "enable_socks5": false,
    "enable_udp_associate": false,
    "max_connections": 4096,
    "connect_timeout_ms": 5000,
    "header_timeout_ms": 3000,
    "idle_timeout_ms": 60000,
    "max_header_bytes": 65536,
    "max_session_buffer_bytes": 1048576,
    "allow_targets": [],
    "deny_targets": []
  }
}
```

If you later want one service to expose multiple proxy protocols, keep protocol-specific config nested under the same root.

## Observability

Minimum recommended metrics:

- active tunnel count
- total accepted connections
- successful upstream connects
- failed upstream connects
- current UDP associations
- bytes in / out
- close reason counters
- timeout counters

Minimum recommended structured logs:

- session accepted
- CONNECT target parsed
- upstream connect success/failure
- session established
- session closed with duration and byte counters
- overflow / timeout / policy rejection

## Security and Policy Controls

Before calling it a formal service, add at least basic controls:

- allowlist / denylist for target host:port
- optional bind address restrictions
- max concurrent sessions per client IP
- max UDP associations per client
- target DNS resolution policy

Longer term:

- auth
- audit log hooks
- tenant / namespace isolation

## Migration Strategy

Recommended migration sequence:

1. Keep `proxy_tool` unchanged as a verification utility.
2. Implement `ProxyService` for HTTP CONNECT only.
3. Add test coverage for lifecycle, connect success/failure, timeout, shutdown.
4. Wire service into `server/services`.
5. Reuse or wrap `Socks5Server`.
6. Add unified config and observability.
7. Only then consider removing duplicated ad hoc logic from `proxy_tool`.

This reduces risk because the debug tool remains available while the formal service stabilizes.

## Testing Strategy

### Unit tests

- CONNECT target parsing
- request header validation
- config validation
- session state transitions
- close/error handling
- timeout firing

### Integration tests

- HTTP CONNECT to reachable upstream
- HTTP CONNECT to unreachable upstream
- malformed request handling
- concurrent tunnel establishment
- idle timeout close
- graceful service stop while tunnels exist

### Existing assets to reuse

- test executable pattern used in `test/protocol/http`
- SOCKS5 protocol coverage already present in `test/protocol/socks5`
- `proxy_tool` can remain a smoke-test client even after service extraction

## Risks

Main risks:

- forcing thread-style tunnel code directly into the runtime without a clean session abstraction
- underestimating backpressure/memory growth during long-lived tunnels
- mixing formal service logic with probe/debug-only helpers
- implementing UDP relay before TCP lifecycle model is solid

Recommended mitigation:

- introduce a session object first
- ship CONNECT-only service first
- add explicit resource limits before broadening capability

### Additional low-level findings (2026-04 proxy integration debugging)

Observed during integration-test failure analysis:

- `AsyncReadAwaiter::ProxyHandler` currently completes the awaiter and then forwards `on_input_shutdown/on_error` to `next_`.
- In common accept paths, `next_` is a default handler that may aggressively call `conn->close()` on shutdown/error.
- This composition creates a real risk window where await completion and default close policy interact in surprising ways under timing pressure.
- `TcpConnection::do_close()` cleanup is queued into the event loop, so close logs/effects can appear delayed relative to the original trigger point.
- `Channel::disable_all()` removes the fd from `SelectPoller` tracking via `update_channel`, which is correct but easy to misuse during temporary state transitions.

Important conclusion from this incident:

- The immediate integration-test failure was not caused by proxy core logic.
- The direct root cause was test-harness behavior on Windows (`SO_RCVTIMEO/SO_SNDTIMEO` type mismatch and short-read stop condition).

Follow-up recommendation (deferred hardening, not emergency fix):

1. Add a guarded awaiter policy so shutdown/error forwarding is optional once awaiter completion is decided.
2. Add a focused regression test for awaiter+default-handler composition under close/error races.
3. Keep event-loop/poller semantics unchanged unless a concrete functional regression is reproduced.

### Additional low-level findings (2026-04 mainline round-two)

Observed while hardening proxy integration and coroutine awaiters:

- Event dispatch generation filtering in `EventLoop` could drop events with generation `0`, which blocked the proxy accept path and produced `accepted=0` under integration tests.
- The immediate fix is to only enforce generation match when `event.generation != 0`, preserving compatibility with pollers that do not carry generation.
- Connection ownership semantics were improved with a lightweight `ConnectionRef` (borrowed or owned), replacing scattered `yuan::base::owner_ptr(...)` access in key coroutine/context paths.
- This reduced accidental raw-pointer usage across suspend boundaries and made awaiter/context APIs explicit about lifetime intent.
- The large CONNECT tunnel behavior on Windows is now treated as a half-close smoke scenario rather than a strict full-echo assertion. A strict no-half-close large tunnel check passes, while immediate client half-close remains timing-sensitive at the socket/read boundary.

Follow-up recommendations:

1. Keep `ConnectionRef` expansion on remaining service-facing relay state and high-frequency async facades until all suspend-crossing paths are owner-aware.
2. Keep separate tests for strict large-tunnel relay without half-close and half-close smoke behavior so platform socket timing does not masquerade as a core proxy regression.
3. Keep accept-path instrumentation available behind debug-level logging for fast regression triage, but do not retain always-on noisy traces in release-oriented defaults.

## Recommendation Summary

Recommended implementation order:

1. Create a dedicated `ProxyService` under `server/services`.
2. Implement HTTP CONNECT as the first formal capability.
3. Model each tunnel as a runtime-managed session object.
4. Reuse existing `Socks5Server` rather than rewriting SOCKS5 behavior.
5. Add UDP service hardening only after TCP tunnel service is stable.

In short: yes, it is feasible, and the repo already has enough runtime and service infrastructure to do it cleanly. The right path is service extraction and composition, not incremental hardening of `proxy_tool` into the final service artifact.
