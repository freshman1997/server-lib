# Network Runtime Refactor Plan

## Goal

Reduce how much low-level network/runtime detail leaks into protocol modules and service entrypoints.

The target is not to replace the current reactor model.
The target is to make the reactor/runtime boundary explicit, reusable, and hard to misuse, so protocol code can focus on request/response or session behavior instead of socket/event-loop ownership details.

## Current Problem

Today, too many server/client implementations still directly handle:

- `EventLoop / Poller / TimerManager` ownership
- acceptor/connection/channel lifecycle
- timeout timer setup and cleanup
- manual `connect -> wait -> parse -> cleanup` orchestration
- callback style and coroutine style bridging inside protocol code
- transport-specific setup details leaking into business-facing code

That means protocol and service code is still carrying infrastructure concerns that should live below it.

## Concrete Leakage Points

### Protocol-side leakage

- `DnsClient` and `UdpTracker` both have to know how to assemble their own datagram runtime, wire acceptors, attach transport instances, send packets, wait, and clean up.
- `HttpClient` still owns a lot of connection/runtime orchestration instead of exposing a cleaner request-session abstraction.
- Multiple protocols still mix:
  - direct event-loop driving
  - `sync_wait(...)`
  - completion-event waiting
  - callback-based request completion

### Service-side leakage

- `HttpService`, `FtpService`, `DnsService`, and similar service wrappers still tend to create a thread and call `server_->serve()`.
- Service code still knows too much about how the underlying runtime is hosted, stopped, and joined.

### Style leakage

- There is still no single dominant async shape across the repository.
- Some modules are scheduler-native.
- Some are callback-first with coroutine bridges.
- Some still use thread + blocking wait boundaries.

## Refactor Principles

- Keep the reactor foundation.
- Hide raw runtime ownership from most protocol code.
- Let protocol code describe intent, not transport mechanics.
- Make coroutine-facing APIs first-class, with sync wrappers at the edge.
- Migrate by sample path, not by mass rewrite.
- Keep tests green at every stage.

## Target Layering

### Layer 1: Raw Net Runtime

This layer stays close to the metal and remains implementation-oriented.

Examples:

- `Socket`
- `Channel`
- `Poller`
- `EventLoop`
- `Acceptor`
- `Connection`
- timer internals

This layer should remain available, but most protocol/service code should no longer depend on it directly.

### Layer 2: Async Primitives

This is the key missing middle layer.

It should expose reusable async/runtime building blocks such as:

- connect awaitable
- accept awaitable
- readable/writable/closed/error awaitables
- request timeout/cancellation primitive
- flush/close completion primitive
- datagram send/receive primitive
- stream read/write primitive
- unified completion object and runtime host contract

This layer should be the common base for both:

- coroutine-native APIs
- compatibility sync wrappers

### Layer 3: High-level Network Facades

This layer gives protocol code a stable, narrow surface.

Candidate abstractions:

- `AsyncClientSession`
- `AsyncRequestClient`
- `AsyncDatagramClient`
- `AsyncListenerHost`
- `AsyncConnectionContext`
- `ServerRuntimeHost`

Protocol code should mostly say:

- how to build a request
- how to parse a response
- how to transition protocol state

It should not manually manage runtime plumbing.

## What Should Stop Appearing In Protocol Code

Long-term, most protocol modules should no longer directly:

- `new` or `delete` pollers/event loops/timer managers
- build acceptors manually for normal request workflows
- attach datagram instances by hand
- mix runtime setup, I/O driving, timeout wiring, and protocol parsing in one class
- choose between callback/coroutine/blocking style inside the protocol core

Those concerns should move into shared runtime or async facade layers.

## What Should Stay In Protocol Code

- packet/message encoding and decoding
- protocol-specific state machines
- request/response semantics
- retry policy or sequencing rules when they are protocol-specific
- protocol-specific session state

## Recommended Migration Order

### Phase 1: Define the boundary

Document and freeze which responsibilities belong to:

- raw net runtime
- async primitives
- protocol/session layer
- service hosting layer

This phase is mostly design and naming.

### Phase 2: Pick two protocol samples

Use one datagram sample and one stream/request sample.

Recommended:

1. `UdpTracker`
2. `HttpClient`

Why:

- `UdpTracker` is small, bounded, and good for datagram runtime extraction.
- `HttpClient` is the best pressure test for request-session abstractions and callback/coroutine unification.

### Phase 3: Stabilize coroutine-first shapes

For each sample:

- build a native coroutine API
- keep a sync compatibility wrapper at the edge
- move runtime ownership/setup out of the protocol core where possible
- add a dedicated focused regression test

### Phase 4: Lift service hosting

After protocol/client-side patterns stabilize, move service wrappers toward a shared hosting abstraction:

- common start/stop/run lifecycle
- explicit runtime ownership
- unified shutdown path
- less per-service thread boilerplate

### Phase 5: Broaden to remaining protocols

Once the sample path is stable, expand to:

- DNS
- FTP
- WebSocket
- Redis / RPC consumers

## Suggested Milestones

### Milestone A: Async primitive completeness

Minimum target:

- connection event awaitables
- timeout/completion primitives
- datagram request/response helper
- stream request/response helper

### Milestone B: Request-client family

Minimum target:

- shared request client/session contract
- `HttpClient` and similar consumers stop managing raw runtime details themselves

### Milestone C: Datagram-client family

Minimum target:

- shared datagram request contract
- `DnsClient` and `UdpTracker` share more runtime/transport machinery

### Milestone D: Service runtime host

Minimum target:

- server/service wrappers no longer each reinvent runtime ownership and thread hosting

## Immediate Candidate Work

If continuing from the current repository state, the next most valuable targets are:

1. Harden `HttpClient::connect_async(...)`
   This is currently the main blocker for moving more stream/request workflows onto the native coroutine side safely.

2. Extract shared datagram client runtime helpers
   `DnsClient` and `UdpTracker` still share obvious setup/cleanup patterns that should become common infrastructure.

3. Define a service runtime host abstraction
   `server/services/*` is still too close to raw server lifecycle control.

## Risks

### Risk: abstracting too early

If abstractions are extracted before two real samples agree on shape, the result will likely be generic but wrong.

Mitigation:

- require at least two protocol samples before freezing a public shared abstraction

### Risk: breaking Windows/MinGW behavior

The repository currently exposes real lifecycle and heap-sensitivity issues more easily on Windows.

Mitigation:

- treat Windows smoke tests as first-class gating
- prefer stable compatibility wrappers over half-working coroutine APIs

### Risk: mixing incompatible async styles

If callback and coroutine shapes continue to evolve independently, complexity will grow instead of shrinking.

Mitigation:

- define coroutine-native internals first
- let sync/callback APIs become wrappers where practical

## Success Criteria

This refactor is succeeding when:

- protocol code gets smaller and more declarative
- new protocol implementations no longer need to understand raw runtime ownership
- service wrappers become thinner
- coroutine style stops fragmenting across modules
- focused smoke tests cover each new async facade

## Current Snapshot

At the moment:

- the reactor/runtime base is still the right foundation
- coroutine primitives exist, but the family is incomplete
- `UdpTracker` is now a valid coroutine-side sample
- `HttpClient` remains the main blocker for a broader stream/request-side migration
- server-side hosting still needs its own abstraction pass

## Recommended Next Session

When resuming later, start here:

1. inspect `HttpClient::connect_async(...)` lifetime and cleanup model
2. decide whether to fix `HttpClient` first or extract datagram runtime helpers first
3. do not expand coroutine adoption to more stream protocols until `HttpClient` is stable
