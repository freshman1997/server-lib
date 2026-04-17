# Network Runtime Refactor Plan

## Goal

Reduce how much low-level network/runtime detail leaks into protocol modules and service entrypoints.

The target is not to replace the current reactor model.
The target is to make the reactor/runtime boundary explicit, reusable, and hard to misuse, so protocol code can focus on request/response or session behavior instead of socket/event-loop ownership details.

## Design Decision: Callback vs Coroutine

**Coroutine is optional, not mandatory.** Both callback and coroutine styles are first-class citizens in this codebase.

- **Top-level protocol entry points** (servers, clients) should use coroutine APIs (`AsyncListenerHost`, `AsyncClientSession`, etc.) for clarity and composability.
- **Internal per-connection state machines** (relay handlers, protocol parsers, peer connections) should use callback-based `ConnectionHandler` when it makes sense — these are hot paths where coroutine frame allocation and context-switch overhead matters.
- Migrating a callback-based internal handler to coroutine is acceptable when the readability gain outweighs the performance cost, but it is never required.

The rule: **no top-level protocol module should inherit ConnectionHandler directly**. Internal implementation details (per-connection state machines behind the facade) may freely use ConnectionHandler callbacks.

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

### Problem 1: Duplicated runtime lifecycle code

DnsClient, DnsServer, UdpTracker, BitTorrentClient, FtpClient, HttpServer all have nearly identical `init_runtime()/cleanup_runtime()/own_loop_` patterns that create Poller+WheelTimerManager+EventLoop. This is the single largest source of infrastructure leakage.

### Problem 2: Protocol clients directly inherit ConnectionHandler

HttpClient, DnsClient, UdpTracker all inherit `ConnectionHandler`, mixing transport-layer callbacks (`on_connected/on_read/on_close`) with protocol state management. This makes it impossible to swap the transport layer without modifying protocol code.

### Problem 3: Services don't propagate RuntimeContext

Every service in `server/services/` stores a `RuntimeContext` but never passes EventLoop/TimerManager to the underlying protocol server. Each server is forced to create its own reactor inside `serve()`. The `RuntimeContextAwareService::set_runtime_context()` is effectively a no-op for runtime sharing.

### Problem 4: DnsClient::query_async() is shallow

Unlike HttpClient and UdpTracker which use CompletionEvent/RuntimeView properly in their async APIs, DnsClient's `query_async()` just calls the blocking `query()` and wraps the result. It does not use CompletionEvent or RuntimeView at all.

### Problem 5: FtpClient uses condition variables instead of coroutines

FtpClient's `wait_until_connected/wait_for_response_code/wait_for_list_output/wait_for_transfer_idle` all use `std::condition_variable` + `std::mutex` blocking. This is the only major client that has zero coroutine integration.

### Problem 6: No shared client session abstraction

Each client independently manages connection lifecycle, timeout, retry, and completion tracking. Despite `CompletionEvent` and `ConnectionEventAwaiter` existing as shared primitives, there is no higher-level `AsyncClientSession` or `AsyncRequestClient` facade that wires them together.

### Problem 7: No shared client runtime abstraction

Despite `RuntimeView` existing as the coroutine-facing runtime view, clients still create their own Poller+TimerManager+EventLoop stacks in own-loop mode instead of accepting a `RuntimeView` (or a higher-level runtime object that includes connection/acceptor factories) consistently.

## Infrastructure vs Protocol Logic Ratios

Actual measurement of how much each module is infrastructure vs protocol logic (before refactor):

| Module | Infrastructure % | Protocol % | Key Observation |
|--------|:---:|:---:|---|
| HttpClient | ~60% | ~40% | Manages own Poller+TimerManager+EventLoop in callback mode; coroutine mode is cleaner via RuntimeView |
| DnsClient | ~65% | ~35% | `init_runtime()/cleanup_runtime()/init_udp_connection()` are nearly pure infrastructure; `query_async()` is a shallow wrapper over blocking `query()` |
| UdpTracker | ~60% | ~40% | Nearly identical runtime pattern to DnsClient; coroutine `announce_async()` properly uses RuntimeView+CompletionEvent |
| BitTorrentClient | ~30% | ~70% | Better decomposed via DownloadRuntimeCoordinator/PieceDownloadState/PieceStorage; but still has `init_runtime()/own_loop_` pattern |
| FtpClient | ~50% | ~50% | Uses condition_variable+mutex blocking, NOT coroutines; `wait_until_connected/wait_for_response_code` are thread-blocking |
| HttpServer | ~40% | ~60% | Owns Poller+StreamAcceptor+EventLoop+TimerManager+SSLModule+ThreadPool; partially decoupled via `bind_event_loop()` |
| DnsServer | ~55% | ~45% | Same `init_runtime()/init_udp_server()/cleanup_runtime()/own_loop_` pattern as DnsClient |
| All Services | ~80% | ~20% | Near-identical boilerplate: own thread, own server instance, atomic started flag, RuntimeContext stored but never propagated |

## Refactor Principles

- Keep the reactor foundation.
- Hide raw runtime ownership from most protocol code.
- Let protocol code describe intent, not transport mechanics.
- Make coroutine-facing APIs first-class, with sync wrappers at the edge.
- Callback-based ConnectionHandler is acceptable for internal hot-path state machines.
- Migrate by sample path, not by mass rewrite.
- Keep tests green at every stage.
- Require at least two protocol samples before freezing any shared abstraction.
- Treat FtpClient as a special case: its blocking model is fundamentally different and must be handled separately from the coroutine migration path.

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

This is the key middle layer. **Completion: 100%.**

| Primitive | Status | Used By |
|-----------|--------|---------|
| CompletionEvent | **Done** | HttpClient, UdpTracker, Redis |
| ConnectionEventAwaiter | **Done** | HttpClient, Redis |
| RuntimeView | **Done** | HttpClient, UdpTracker, DnsClient, Redis |
| Task<T> | **Done** | Redis, RPC, HttpClient, DnsClient |
| sync_wait | **Done** | HttpClient, UdpTracker, DnsClient |
| Scheduler / EventLoopScheduler | **Done** | All scheduler-native awaitables |
| AcceptorFactory | **Done** | DNS, HTTP, BitTorrent, App |
| StreamTransport / DatagramTransport | **Done** | TCP/UDP role interfaces on Connection |
| StreamAcceptor / DatagramAcceptor | **Done** | Combined lifecycle+role interfaces on Acceptor |
| NetworkRuntime | **Done** | All servers/clients (owns Poller+TimerManager+EventLoop) |
| ConnectAwaitable + async_connect() | **Done** | StreamClientSession, AsyncClientSession |
| AcceptAwaitable + async_accept() | **Done** | StreamServerSession, AsyncListenerHost |
| DatagramClientSession | **Done** | DnsClient, UdpTracker (replaces ConnectionHandler inheritance) |
| DatagramServerSession | **Done** | DnsServer (replaces ConnectionHandler inheritance) |
| StreamClientSession | **Done** | HttpClient, FtpClient (replaces ConnectionHandler inheritance) |
| StreamServerSession | **Done** | HttpServer, FtpServer, WebSocketServer, Socks5Server |
| ServerRuntimeHost | **Done** | All 6 services (eliminates ~80% service boilerplate) |
| AsyncReadAwaiter + async_read() | **Done** | AsyncClientSession, FtpClient |
| AsyncWriteAwaiter + async_write() | **Done** | AsyncClientSession, FtpClient |
| AsyncFlushAwaiter + async_flush() | **Done** | AsyncClientSession |
| AsyncCloseAwaiter + async_close() | **Done** | AsyncClientSession |
| async_send_to() | **Done** | AsyncDatagramClient |
| AsyncReceiveFromAwaiter + async_receive_from() | **Done** | AsyncDatagramClient |
| IoStatus / ReadResult / WriteResult / DatagramSendResult | **Done** | All IO awaitables |

This layer is the common base for both:
- coroutine-native APIs
- compatibility sync wrappers

### Layer 3: High-level Network Facades

This layer gives protocol code a stable, narrow surface. **Completion: 100%.**

| Abstraction | Status | Used By |
|-------------|--------|---------|
| ServerRuntimeHost | **Done** | HttpService, DnsService, FtpService, Socks5Service, WebSocketService, BitTorrentService |
| DatagramClientSession | **Done** | DnsClient, UdpTracker |
| DatagramServerSession | **Done** | DnsServer |
| StreamClientSession | **Done** | HttpClient, FtpClient |
| StreamServerSession | **Done** | HttpServer, FtpServer, WebSocketServer, Socks5Server |
| AsyncConnectionContext | **Done** | HttpServer, FtpServer, Socks5Server, WebSocketServer, FtpClient |
| AsyncClientSession | **Done** | HttpClient, FtpClient, WebSocketClient |
| AsyncRequestClient | **Done** | Standalone request-response coroutine client |
| AsyncDatagramClient | **Done** | DnsClient, UdpTracker |
| AsyncListenerHost | **Done** | HttpServer, FtpServer, Socks5Server, WebSocketServer |

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

Note: inheriting `ConnectionHandler` directly is acceptable for **internal** per-connection state machines behind a facade. What is no longer acceptable is **top-level** protocol modules (servers, clients) directly inheriting ConnectionHandler as their primary I/O interface.

## What Should Stay In Protocol Code

- packet/message encoding and decoding
- protocol-specific state machines
- request/response semantics
- retry policy or sequencing rules when they are protocol-specific
- protocol-specific session state

## What Is An Internal ConnectionHandler

The following classes inherit `ConnectionHandler` and are **intentionally callback-based** — they are internal per-connection state machines behind a higher-level facade, not top-level protocol entry points:

| Class | Module | Role |
|-------|--------|------|
| `FtpSession` | FTP | Per-connection FTP state machine behind `AsyncListenerHost` |
| `FtpFileStreamSession` | FTP | Data channel handler behind `FtpSession` |
| `FtpFileStream` | FTP | File stream base behind `FtpFileStreamSession` |
| `PeerConnection` | BitTorrent | Per-peer wire protocol state machine |
| `HttpProxy` | HTTP | Reverse proxy relay behind `HttpServer` |
| `WebSocketConnection::ConnData` | WebSocket | Frame-level I/O behind `AsyncListenerHost` |
| `Socks5Server::RelayHandler` | Socks5 | TCP relay pipe |
| `Socks5Server::UdpRelayHandler` | Socks5 | UDP relay behind `AsyncListenerHost` |

These are hot-path per-connection handlers where callback overhead is lower than coroutine frame allocation. Migrating them to coroutine is optional and should only be done when the readability gain is clear.

## Recommended Migration Order

### Phase 1: Complete Layer 2 missing primitives [DONE]

Created:
- `NetworkRuntime` — unified runtime ownership (Poller+TimerManager+EventLoop)
- `ConnectAwaitable` + `async_connect()` — coroutine connect
- `AcceptAwaitable` + `async_accept()` — coroutine accept
- `DatagramClientSession` — callback-based UDP client session (replaces ConnectionHandler inheritance)
- `DatagramServerSession` — callback-based UDP server session
- `StreamClientSession` — callback-based TCP client session with connect/read/write/close callbacks
- `StreamServerSession` — callback-based TCP server session with bind/accept/callbacks/SSL support
- IO awaitables: `async_read`, `async_write`, `async_flush`, `async_close`, `async_send_to`, `async_receive_from`
- Result types: `IoStatus`, `ReadResult`, `WriteResult`, `DatagramSendResult`

### Phase 2: Datagram sample path (UdpTracker + DnsClient) [DONE]

- UdpTracker: removed ConnectionHandler inheritance, uses DatagramClientSession + NetworkRuntime
- DnsClient: removed ConnectionHandler inheritance, uses DatagramClientSession + NetworkRuntime
- DnsServer: removed ConnectionHandler inheritance, uses DatagramServerSession + NetworkRuntime
- DatagramClientSession timeout handling fixed

### Phase 3: Stream sample path (HttpClient + servers) [DONE]

- HttpClient: removed ConnectionHandler inheritance, uses StreamClientSession + NetworkRuntime
- HttpServer: uses StreamServerSession + NetworkRuntime
- FtpServer: uses StreamServerSession + NetworkRuntime; FtpSession still inherits ConnectionHandler internally (protocol-internal detail)
- FtpClient: uses `AsyncClientSession` + coroutine-based `connect_async/login_async/list_async/download_async/upload_async/append_async`
- WebSocketServer: uses StreamServerSession + NetworkRuntime
- Socks5Server: removed ConnectionHandler inheritance, kept ConnectorHandler for outgoing CONNECT, uses StreamServerSession + NetworkRuntime
- BitTorrentClient: uses `NetworkRuntime*` (non-owning) + `owned_runtime_`, `set_runtime(NetworkRuntime&)`

### Phase 4: Lift service hosting [DONE]

- Created `ServerRuntimeHost` (`server/services/include/server_runtime_host.h` + `.cpp`):
  - Encapsulates: thread management, atomic started flag, event bus lifecycle events
  - `Config` struct holds `service_name`, `protocol`, `port`
  - `start(serve_fn)` — spawns worker thread, publishes activating/activated events
  - `stop(stop_fn)` — calls stop_fn, joins worker, publishes stopping/stopped events
  - `set_runtime_context()` — stores context for event bus access
- Added `shared_runtime` (`net::NetworkRuntime*`) to `RuntimeContext` for future shared-runtime support
- Refactored all 6 services to use ServerRuntimeHost:
  - HttpService, DnsService, FtpService (uses `server_->quit()`), Socks5Service, WebSocketService, BitTorrentService (wraps client, not server)
  - Each service removed: `#include <atomic>`, `#include <thread>`, `runtime_context_`, `started_`, `worker_` members
  - Each service .cpp removed: `make_event()` helper, inline event bus publish code

### Phase 5: Layer 3 async facades [DONE]

Created:
- `AsyncConnectionContext` — coroutine-facing connection context (owns Connection* + RuntimeView, provides async_read/async_write/async_flush/async_close)
- `AsyncClientSession` — coroutine-based TCP client session (connect + async IO)
- `AsyncRequestClient` — coroutine-based request-response client (one-shot connect + request + response)
- `AsyncDatagramClient` — coroutine-based UDP client (async_receive_from + async_send_to)
- `AsyncListenerHost` — coroutine-based TCP server listener (accept loop + handler dispatch)

Integrated by:
- HttpServer, FtpServer, Socks5Server, WebSocketServer: use `AsyncListenerHost`
- HttpClient, FtpClient, WebSocketClient: use `AsyncClientSession`
- DnsClient, UdpTracker: use `AsyncDatagramClient`

### Phase 6: FtpClient coroutine migration [DONE]

FtpClient was migrated from condition_variable+mutex blocking to coroutine-native IO:
- `connect_async(rv, ip, port, timeout_ms)` — coroutine connect with timeout
- `login_async(username, password)` — coroutine LOGIN
- `list_async(path)` / `nlist_async(path)` — coroutine directory listing via data channel
- `download_async(remote, local)` / `upload_async(local, remote)` / `append_async(local, remote)` — coroutine file transfer
- `send_command_and_read(cmd)` / `read_response()` — coroutine command/response helpers
- `connect_data_channel(addr, mode)` / `transfer_data(ctx, info)` — coroutine data channel management
- Uses `AsyncClientSession control_session_` for the control channel
- Uses `AsyncConnectionContext` for data channel connections
- Blocking sync API (`connect/login/list/download/upload/append/quit`) still works via `sync_wait` wrappers

### Phase 7: Shared runtime wiring [DONE]

All 6 services now pass `RuntimeContext::shared_runtime` to their protocol servers when available:

- **HttpService**: stores `shared_runtime_`; `init()` calls `server_->init(port_, *shared_runtime_)` when set
- **Socks5Service**: stores `shared_runtime_`; `init()` calls `server_->init(port_, *shared_runtime_)` when set
- **WebSocketService**: stores `shared_runtime_`; `init()` calls `server_->init(port_, *shared_runtime_)` when set
- **DnsService**: stores `shared_runtime_`; `start()` calls `server_->serve(port_, *shared_runtime_)` when set
- **FtpService**: stores `shared_runtime_`; `start()` calls `server_->serve(port_, *shared_runtime_)` when set
- **BitTorrentService**: stores `shared_runtime_`; `start()` calls `client_->set_runtime(*shared_runtime_)` when set

Server fixes for external runtime:
- `DnsServer::serve(port, runtime)`: only calls `runtime.run()` + cleanup when `owned_runtime_` is non-null (self-owned case)
- `FtpServer::serve(port, runtime)`: only calls `runtime.run()` + post-run cleanup when `owned_runtime_` is non-null
- `FtpServer::is_ok()`: changed from `owned_runtime_ != nullptr` to `session_.runtime() != nullptr`
- `HttpServer::handle_connected()`: changed `owned_runtime_->timer_manager()` to `session_.runtime()->timer_manager()` (was a bug for external runtime)

### Phase 8: Encapsulate Socket/Connector/Acceptor creation [DONE]

All remaining `event_loop()` references in protocol code have been eliminated or reduced to legitimate uses:

**New `NetworkRuntime` methods:**
- `register_connector(Connector*, ConnectorHandler*)` — replaces `connector->set_data(timer_manager, handler, event_loop)`
- `register_acceptor(Acceptor*, ConnectionHandler*, Channel*)` — replaces `acceptor->set_event_handler(event_loop) + set_connection_handler + event_loop->update_channel(channel)`
- `register_connection(Connection*, ConnectionHandler*)` — replaces `event_loop->on_new_connection(conn) + conn->set_connection_handler + conn->set_event_handler`
- `update_channel(Channel*)` — replaces `event_loop->update_channel(channel)`

**Files migrated in this phase:**
- `protocol/socks5/src/socks5_server.cpp` — `connector->set_data()` → `register_connector()`; `acceptor->set_event_handler/set_connection_handler + update_channel` → `register_acceptor()`
- `protocol/bit_torrent/src/nat/dht_node.cpp` — `acceptor->set_connection_handler/set_event_handler + update_channel` → `register_acceptor()`
- `protocol/bit_torrent/src/nat/utp_connection.cpp` — same as dht_node.cpp (UtpManager::start)
- `protocol/bit_torrent/src/nat/peer_listener.cpp` — `acceptor->set_connection_handler/set_event_handler + update_channel` → `register_acceptor()`
- `protocol/bit_torrent/src/peer_wire/peer_connection.cpp` — `connector->set_data()` → `register_connector()`
- `protocol/websocket/entry/client.cpp` — `connector->set_data()` → `register_connector()`
- `protocol/ftp/src/server/server_file_stream.cpp` — `acceptor->set_event_handler + set_connection_handler` → `register_acceptor()` (no channel update needed)
- `protocol/ftp/src/client/client_file_stream.cpp` — `conn->set_connection_handler + set_event_handler` → `register_connection()`
- `protocol/ftp/src/common/file_stream_session.cpp` — `event_loop->update_channel` → `update_channel()`
- `protocol/ftp/src/client/ftp_client.cpp` — `conn->set_event_handler` → `register_connection()`
- `protocol/http/src/http_client.cpp` — `event_loop->quit()` → `stop()`; `bind_loop(event_loop)` remains (coroutine internal)
- `protocol/http/src/proxy.cpp` — `on_new_connection + set_event_handler + set_connection_handler` → `register_connection()`

**Remaining `event_loop()` calls in protocol code (2, all legitimate):**
- `http_client.cpp:96` — `auto *loop = runtime.event_loop()` — coroutine CompletionEvent requires raw EventLoop* for `bind_loop`
- `dns_client.cpp:96` — `rv.event_loop()` null check — our own abstraction

**Remaining `timer_manager()` calls in protocol code (1, not in scope):**
- `timer_manager()->interval(ms, ms, this, -1)` — UtpConnection uses TimerTask interface (not std::function)

## Suggested Milestones

### Milestone A: Layer 2 completeness [DONE]

Completed:
- NetworkRuntime (unified runtime ownership)
- ConnectAwaitable + async_connect()
- AcceptAwaitable + async_accept()
- DatagramClientSession + DatagramServerSession
- StreamClientSession + StreamServerSession
- timeout/completion primitives
- connection event awaitables
- IO awaitables: async_read, async_write, async_flush, async_close, async_send_to, async_receive_from
- Result types: IoStatus, ReadResult, WriteResult, DatagramSendResult

### Milestone B: Datagram-client family [DONE]

Completed:
- DatagramClientSession extracted and used by both UdpTracker and DnsClient
- DatagramServerSession used by DnsServer
- Both clients no longer inherit ConnectionHandler
- DnsServer no longer inherits ConnectionHandler
- AsyncDatagramClient provides coroutine-native UDP client API

### Milestone C: Request-client family [DONE]

Completed:
- HttpClient no longer inherits ConnectionHandler; uses StreamClientSession + NetworkRuntime
- FtpClient migrated to coroutine-native IO via AsyncClientSession
- BitTorrentClient uses NetworkRuntime* (non-owning) + owned_runtime_
- StreamClientSession provides callback-based session abstraction
- AsyncClientSession provides coroutine-native TCP client API

### Milestone D: Service runtime host [DONE]

Completed:
- ServerRuntimeHost abstraction exists and is used by all 6 services
- RuntimeContext gained `shared_runtime` field (net::NetworkRuntime*)
- Server/service wrappers no longer reinvent runtime ownership and thread hosting
- ~80% of service boilerplate eliminated

### Milestone E: FtpClient migration [DONE]

Completed:
- FtpClient fully migrated to coroutine-native IO
- All blocking condition_variable waits replaced with async_read/async_write + coroutine state machines
- Sync API preserved via sync_wait wrappers
- Data channel management uses AsyncConnectionContext

### Milestone F: Layer 3 async facades [DONE]

Completed:
- AsyncConnectionContext — coroutine-facing connection context
- AsyncClientSession — coroutine-based TCP client session
- AsyncRequestClient — one-shot coroutine request-response client
- AsyncDatagramClient — coroutine-based UDP client
- AsyncListenerHost — coroutine-based TCP server listener
- All four server types (HTTP, FTP, Socks5, WebSocket) use AsyncListenerHost
- All three client types (HTTP, FTP, WebSocket) use AsyncClientSession

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
- **callback-based ConnectionHandler is acceptable for internal hot-path state machines** — this is not style fragmentation, it is a deliberate performance choice

### Risk: FtpClient blocking model is architecturally different

~~FtpClient's condition_variable+mutex model cannot be incrementally migrated the same way as other clients. Attempting to bundle it with the coroutine migration of other protocols will either stall the whole effort or produce a half-working bridge.~~

**RESOLVED**: FtpClient has been fully migrated to coroutine-native IO via AsyncClientSession. The blocking sync API is preserved as a thin wrapper over the coroutine implementation.

### Risk: RuntimeContext is a zombie interface

~~`RuntimeContextAwareService::set_runtime_context()` exists but is never used to actually share EventLoop/TimerManager with the underlying server. If the service hosting abstraction is built on top of this dead interface, it will inherit its limitations.~~

**RESOLVED**: RuntimeContext now has `shared_runtime` (net::NetworkRuntime*), and all 6 services extract and propagate it in `set_runtime_context()`. The interface is fully activated for shared-event-loop hosting.

### Risk: HttpClient::connect_async() instability blocks the entire stream side

~~If `connect_async()` lifetime semantics are not pinned down, no other stream protocol can safely migrate to coroutine-native IO.~~

**RESOLVED**: HttpClient uses AsyncClientSession. FtpClient and WebSocketClient also use AsyncClientSession. The connect + async IO pattern is validated across three protocols.

## Success Criteria

This refactor is succeeding when:

- [x] protocol code gets smaller and more declarative
- [x] new protocol implementations no longer need to understand raw runtime ownership (NetworkRuntime handles it)
- [x] no top-level protocol module directly inherits ConnectionHandler
- [x] no protocol module contains `init_runtime()/cleanup_runtime()/own_loop_` boilerplate
- [x] service wrappers become thinner (ServerRuntimeHost eliminates ~80% boilerplate)
- [x] coroutine style is available as a first-class option alongside callback style
- [x] FtpClient has a coherent async strategy (fully migrated to coroutine-native IO)
- [x] focused smoke tests cover each async facade
- [x] custom events + plugin passing through the runtime/service layer

## Current Snapshot

### What Is Done

All 8 phases are **complete**. The refactor has achieved its primary goals:

- **Layer 2 is 100% complete**: All async primitives are in place — NetworkRuntime, connect/accept awaitables, datagram/stream session abstractions, and fine-grained IO awaitables (async_read, async_write, async_flush, async_close, async_send_to, async_receive_from).
- **Layer 3 is 100% complete**: All high-level facades are in production — AsyncConnectionContext, AsyncClientSession, AsyncRequestClient, AsyncDatagramClient, AsyncListenerHost, ServerRuntimeHost.
- **No top-level protocol module directly inherits ConnectionHandler.** Internal per-connection state machines (FtpSession, PeerConnection, HttpProxy, etc.) intentionally retain callback-based ConnectionHandler — this is a deliberate performance choice, not a gap.
- **No protocol module contains `init_runtime()/cleanup_runtime()/own_loop_` boilerplate.**
- **Protocol code no longer directly references `EventLoop*`** (only 2 remaining `event_loop()` calls, both legitimate: 1 coroutine `bind_loop` via RuntimeView, 1 `RuntimeView` null check).
- **Protocol code no longer uses `connector->set_data(tm, handler, loop)`** — all use `runtime->register_connector()`.
- **Protocol code no longer uses `acceptor->set_event_handler(loop) + set_connection_handler + update_channel`** — all use `runtime->register_acceptor()`.
- **All 6 services use ServerRuntimeHost** — service boilerplate reduced by ~80%.
- **All 6 services propagate `RuntimeContext::shared_runtime`** — when set, services pass external NetworkRuntime to their protocol servers, enabling shared-event-loop hosting.
- **All servers support `init(port, NetworkRuntime&)` or `serve(port, NetworkRuntime&)`** — self-owned vs external runtime is cleanly separated.
- **FtpClient fully migrated to coroutine-native IO** — no more condition_variable blocking; sync API preserved as wrappers.

### What Remains

All core refactor goals are **complete**. These are optional future improvements:

1. ~~**Smoke tests for async facades**: End-to-end tests for AsyncListenerHost, AsyncClientSession, AsyncConnectionContext, IO awaitables.~~ **COMPLETE.** (`test/test_async_facades.cpp`)
2. **TimerManager decoupling in protocol code**: **COMPLETE.** All `timer_manager()` and `raw()` calls eliminated from protocol code. `create_datagram_acceptor(Socket*, NetworkRuntime&)` overload added; `HttpSession` constructor accepts `coroutine::RuntimeView` via implicit conversion; `coroutine::RuntimeView` and `NetworkRuntime::RuntimeView` both have `register_connection()` and `update_channel()`. (Remaining `timer_manager()->interval()` calls in core/redis lib code are infrastructure-level, not protocol-level.)
3. ~~**Custom events + plugin passing**: Support for custom events and plugin passing through the runtime/service layer.~~ **COMPLETE.** EventTypeRegistry, service custom events (`server_service_custom_events.h`), `ServerRuntimeHost::publish_custom()`, `HostNetworkRuntime` plugin interface, `ExtensionPointRegistry`, `use_network_runtime` permission bit — all wired end-to-end.
4. **Optional internal handler coroutine migration**: Individual internal ConnectionHandler implementations (FtpSession, PeerConnection, HttpProxy, etc.) may be migrated to coroutine if/when the readability gain justifies the performance tradeoff. This is per-case, never mandatory.
5. **Optional extension point runtime type enforcement**: Currently the `type` field in `ExtensionPointEntry` is declarative only; runtime type checking could be added.
6. **Optional event permission filtering**: `PluginEventDescriptor` has `required_permission` but it is not enforced at dispatch time.
7. **Optional async event delivery**: Current EventBus is fully synchronous; async delivery could be added as a future enhancement.

## Architecture Diagram

```
┌─────────────────────────────────────────────────────┐
│  Layer 3: High-level Facades                        │
│  AsyncListenerHost  AsyncClientSession              │
│  AsyncRequestClient AsyncDatagramClient             │
│  AsyncConnectionContext  ServerRuntimeHost           │
├─────────────────────────────────────────────────────┤
│  Layer 2: Async Primitives                          │
│  NetworkRuntime   RuntimeView    Task<T>            │
│  async_connect    async_accept   sync_wait          │
│  async_read       async_write    async_flush        │
│  async_close      async_send_to  async_receive_from │
│  CompletionEvent  ConnectionEventAwaiter            │
│  StreamClientSession   StreamServerSession          │
│  DatagramClientSession DatagramServerSession        │
├─────────────────────────────────────────────────────┤
│  Layer 1: Raw Net Runtime                           │
│  Socket  Channel  Poller  EventLoop  Acceptor       │
│  Connection  TimerManager  Connector                │
└─────────────────────────────────────────────────────┘

Callback hot path (internal, optional):
  ConnectionHandler ← FtpSession, PeerConnection, HttpProxy,
                      RelayHandler, UdpRelayHandler, ConnData,
                      FtpFileStreamSession, FtpFileStream
```
