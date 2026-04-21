# Project Refactor Status

## Conclusion

The project-wide refactor is not fully complete yet.

What is complete now:

- The top-level build has been unified around project options.
- `core/app` is in place as the application composition layer.
- Major service entrypoints have been migrated into the new composition model.
- `server/services` now acts as the service assembly boundary.
- HTTP, WebSocket, FTP, DNS, and match server startup paths can build through the new structure.
- Plugin hosting can now enter the new application lifecycle through an app-level service.
- The previously broken HTTP path has been repaired back to a compilable, evolvable state.

What this means:

- The main refactor backbone is complete.
- The repository can be considered "architecturally on the new mainline".
- But the repository is not yet "fully refactored" according to the original long-term target.

## Verified State

Verified with MinGW:

- Full project build succeeded with:
  - `cmake --build d:/code/src/vs/webserver/cmake-build-mingw`

Current target platforms:

- Primary: MinGW
- Compatible target: Linux
- Compatible target: macOS
- Not a priority in this refactor: MSVC

## Completed Mainline Work

### 1. Build and repository entrypoints

- Top-level `CMakeLists.txt` now owns project-wide switches.
- Optional areas can be built independently:
  - tests
  - plugins
  - libs
  - servers
  - example

### 2. Application composition layer

- `core/app` now provides the shared lifecycle entry for services.
- Core concepts are already introduced:
  - `Application`
  - `Bootstrap`
  - `RuntimeContext`
  - service abstraction

### 3. Service assembly boundary

- Service adapters have been extracted into `server/services`.
- Protocol modules no longer need to directly own app bootstrapping responsibilities.

### 4. Protocol entry migration

- HTTP startup path migrated.
- WebSocket startup path migrated.
- FTP startup path migrated.
- DNS startup path migrated.
- Match server startup path migrated.
- BitTorrent now has a service assembly entry in `server/services`, so it is no longer only a standalone protocol library from the perspective of top-level application composition.
- `BitTorrentClient` has also started internal lifecycle decomposition, with runtime setup/start/stop/cleanup responsibilities beginning to move out of a single monolithic `start/stop` flow.

### 5. HTTP recovery and partial internal refactor

- `HttpServer` lifecycle methods were split into smaller internal steps.
- Request processing began splitting into parse, dispatch, and finalize phases.
- Static file handling, upload handling, directory listing, and OPTIONS preflight were repaired to restore buildability.
- `serve_upload()` is now split into request parsing, upload-session resolution, chunk persistence, and completion-finalization steps.
- `http_server.h` was normalized back into a stable compilable declaration after earlier text corruption, so HTTP can continue evolving without header-level breakage.
- `serve_static()` is now split into static-request resolution, embedded page serving, and file streaming responsibilities, reducing one more large mixed-responsibility path inside `HttpServer`.
- HTTP session lookup, connection-buffer cleanup, and session release paths are now being reused through internal helpers instead of being repeated inline across multiple request lifecycle methods.

### 6. Plugin lifecycle partial migration

- A plugin host service now allows plugins to be loaded through `Application`.
- Plugin loading and unloading can participate in the app service lifecycle.
- Plugins now initialize through a dedicated `PluginContext` instead of the old raw dispatcher-only entry.
- The old `MessageDispatcher`-based plugin message path has now been removed from the plugin mainline, so plugin lifecycle and extension flow no longer depend on the legacy message-consumer model.
- `Application` can inject runtime context into context-aware services before service initialization.
- `PluginContext` can now carry an `EventBus` pointer into plugins.
- `PluginManager` no longer acts as a legacy system-message consumer for plugin load and release requests; it now focuses on library loading, instance creation, initialization, and release.
- `PluginContext` now carries per-plugin identity data such as `plugin_name` and `plugin_root_path`, so plugins can bootstrap against stable host-provided metadata instead of inferring their own identity.
- `PluginContext` now also carries a host log registry pointer, allowing plugins to log through host-provided infrastructure instead of reaching for global logging singletons on their own.
- `PluginContext` now provides a stable plugin config location via `plugin_config_path`, so plugins have a host-defined place to resolve their own configuration without depending on protocol-specific config managers.
- `PluginContext` now also carries a parsed plugin config object when a plugin-specific JSON file exists, giving plugins a minimal host-managed configuration access path without introducing a protocol-bound config dependency.
- `RuntimeContext` now also carries a host-side `ServiceRegistry`, and `PluginContext` now exposes a plugin-facing `HostServiceCatalog` adapter over that registry, which gives plugins a first stable service-discovery boundary without leaking `core/app` internals directly into the plugin SDK.
- The old `MessageDispatcher`-based plugin message path has now been removed from the plugin mainline, so plugin lifecycle and extension flow no longer depend on the legacy message-consumer model.

### 7. Runtime coroutine control cleanup

- `EventLoop` coroutine escape behavior is now expressed with explicit intent instead of a misleading generic boolean flag.
- Coroutine resume signaling now goes through shared runtime/event-loop primitives instead of Redis-specific resume entry points.
- Event loop exit now distinguishes between normal quit and coroutine-resume exit intent.
- Coroutine support is currently real but local: Redis and RPC each have small C++20 coroutine helper wrappers, while the repository still does not have a shared scheduler, awaitable adapters, or a unified coroutine abstraction across modules.
- A shared coroutine `Task<T>` foundation now exists in `Core`, and the existing Redis/RPC helper headers have been reduced to compatibility aliases over that common implementation.
- A first shared event-loop awaitable now exists in `Core`, and the active Redis coroutine call path has been migrated from ad-hoc `suspend_always + loop()` sequencing to that common awaitable bridge.
- Shared timeout and queue-in-loop awaitables now also exist in `Core`, and the Redis message-fetch timeout path has been migrated away from its previous local timer-resume assembly to the common timeout awaitable.
- A lightweight shared `RuntimeView` now exists to bundle `EventLoop` and `TimerManager` into a single coroutine-facing host view, and Redis now consumes that runtime view instead of manually wiring loop/timer access at each coroutine call site.
- A first shared connection-event awaitable now exists in `Core`, and the Redis connect path has begun using that bridge for waiting on connection establishment events instead of relying only on manual event-loop resume wiring.
- Coroutine resume signaling itself is also starting to be routed through `RuntimeView`, reducing direct coroutine-side dependency on Redis-specific registry resume calls.
- A first scheduler foundation now also exists in `Core`: `Scheduler`, `EventLoopScheduler`, `RuntimeView::schedule()`, and `EventLoop::post_coroutine()` are now present, which means the repository no longer depends only on callback queues for coroutine continuation delivery.
- `Task<T>` now supports continuation chaining and direct `co_await`, so coroutine composition is beginning to move beyond "execute one local coroutine object" toward a more mainstream task model.
- A first pair of scheduler-native awaitables now also exists in `Core`: `RuntimeView::dispatch_in_loop()` and `RuntimeView::sleep_for()`. These do not drive `loop()` inline; instead, they resume coroutine handles through the owning `EventLoop` scheduler path.
- A reusable one-shot `CompletionEvent` plus coroutine-side `sync_wait(runtime, task)` boundary now also exists in `Core`, giving the repository a transition path from old bridge-style top-level tasks toward scheduler-native inner await chains.
- `Task<T>` now uses lazy start (`initial_suspend = suspend_always`), which aligns the repository with the mainstream "task starts when scheduled/awaited" coroutine model instead of the earlier eager-start bridge behavior.
- `Task<void>` and `sync_wait(runtime, Task<void>)` now also exist, which closes an important basic coroutine-runtime gap and makes scheduler-native fire-and-wait flows usable without fake boolean return values.
- The remaining legacy bridge awaitables have now been removed from `RuntimeView` and the old bridge-only event-loop awaiter header has been deleted, so new coroutine call paths no longer have those inline-`loop()` shims available from the main runtime facade.
- `FtpClient` now also has a first condition-variable-based wait boundary (`wait_until_connected / wait_for_response_code / wait_for_transfer_idle / wait_for_list_output`), which starts reducing business-layer `sleep + poll` usage on the FTP client side without changing its thread-owned session-loop model.
- `FtpClient` now also provides `wait_for_local_file(...)`, so download-side caller workflows can begin moving off ad-hoc file-existence polling too.
- The FTP interactive/e2e client-side test entries have also started consuming those wait helpers, so that cleanup is no longer limited to the library surface alone.
- `DnsClient::query_async()` has started consuming that new path: packet dispatch and completion/timeout waiting now run through the new scheduler-native primitives behind a `sync_wait` transition boundary.
- `HttpClient::connect_async()` has also started using the new completion path for its response wait phase, while still keeping connection establishment on the older bridge awaitable for now.
- `connection_event_awaitable` has now also started moving to scheduler-native behavior: it no longer drives `EventLoop::loop()` inline inside `await_suspend()`, and HTTP client plus Redis connect paths now enter it through the `sync_wait` transition boundary.
- Redis command execution wait and subscription-message wait have now also started moving away from the old `until_resume()/until_timeout()` bridge path and onto the new `CompletionEvent + sync_wait` transition path.
- A dedicated coroutine runtime smoke test now exists and passes, and that smoke path is now fully scheduler-native instead of mixing in manual `queue_in_loop()` from test code.
- BitTorrent's UDP tracker synchronous announce path has now also started moving off the old `queue_in_loop + loop()` pattern and onto the new `dispatch_in_loop + CompletionEvent + sync_wait` transition path.
- `DnsClient::query()` in own-loop mode no longer relies on the old `queue_in_loop + loop()` bridge path either; it now uses the same scheduler-native transition boundary as `query_async()`.
- BitTorrent's UDP tracker handler-based announce path has now also moved off the old `queue_in_loop + loop()` pattern and onto the new scheduler transition boundary.
- `BitTorrentClient` startup bootstrap in own-loop mode no longer queues its own initial NAT/tracker bootstrap back into the same loop before first run; that startup step now executes directly and only uses `queue_in_loop` when the runtime is externally owned.
- `HttpClient` now also has a coroutine-oriented request path built on top of the shared coroutine runtime primitives, giving the repository a second real business chain beyond Redis that uses the new `Task + RuntimeView + connection-event/timeout awaitable` stack.
- The `http_client` test entry has been migrated from nested callbacks to the new coroutine request API, so the HTTP client path now validates the coroutine mainline through an actual protocol-side executable target.
- `DnsClient` now also has a coroutine-oriented query path built on top of the shared coroutine runtime primitives, giving the repository a UDP-side consumer of the same coroutine mainline instead of limiting that work to TCP-only examples.
- The `dns_client` test entry has been migrated to the coroutine query API, so DNS client behavior now validates the shared coroutine layer through a second non-Redis executable path.
- `DnsClient` has also started internal lifecycle decomposition, with runtime setup, UDP connection setup, wait-completion signaling, and runtime cleanup responsibilities being pulled apart instead of remaining embedded in one large connect/disconnect flow.
- `DnsServer` has also started internal lifecycle decomposition, with runtime setup, UDP listener setup, and runtime cleanup responsibilities now separated instead of being bundled into one large `serve()` implementation.

### 8. Minimal EventBus landing

- A minimal `EventBus` now exists in core runtime support.
- `Application` now ensures a shared event bus is present in runtime context.
- `Application` now publishes lifecycle events for app and service init/start/stop phases.
- `PluginHostService` now publishes plugin load, load-failed, unloading, and unloaded events.
- Network-facing services in `server/services` now publish service activation and stop events with protocol and port metadata.
- BitTorrent now also has a service-layer wrapper in `server/services`, which means the protocol has started entering the same app/service lifecycle boundary as the other major subsystems even though its internal runtime is still much more library-style than the HTTP/FTP/WebSocket/DNS server modules.
- BitTorrent internal runtime ownership is now beginning to be expressed more explicitly instead of being buried entirely inside one large `start()/stop()` implementation, which makes later convergence onto shared runtime patterns more practical.
- BitTorrent startup flow is also beginning to be decomposed beyond runtime setup, with NAT startup, listen-port synchronization, and periodic stats scheduling now separated out of one mixed startup block.
- BitTorrent tracker announce coordination now lives in a dedicated `TrackerSession`, and peer connection lifecycle management now lives in a dedicated `PeerSession`, which separates peer/tracker/session responsibilities from the client orchestrator more cleanly.
- Plugins can subscribe and publish through the event bus without going through the legacy message dispatcher for extension events.

### 9. Buffer mainline migration has started

- A new buffer mainline now exists in `Core`:
  - `ByteBuffer`
  - `BufferChain`
- The new buffer model has its own executable validation target, and the first real protocol parser path has started consuming it.
- DNS packet encode/decode is now built on `ByteBuffer` instead of the legacy `Buffer` type.
- DNS client/server currently keep legacy transport-buffer adaptation only at the network boundary, which means the DNS protocol path is now the first real parser chain entering the new buffer model without forcing a transport-layer rewrite first.
- BitTorrent UDP tracker request/response packing has now also moved onto `ByteBuffer`, with legacy `Buffer` retained only at the transport boundary.
- BitTorrent peer handshake/message accumulation has started moving off legacy `Buffer` consumption too: `PeerConnection` and `PeerListener` now stage inbound bytes in `ByteBuffer` before parsing instead of using legacy buffer read-index/reset semantics directly as the protocol state machine.
- BitTorrent uTP and DHT UDP send paths have also started using `ByteBuffer` as the serialization source before the final transport-boundary adaptation.
- The repository now also has first-stage transport-capability boundaries:
  - `StreamTransport`
  - `DatagramTransport`
- `TcpConnection` and `UdpConnection` now implement those role interfaces, and `EventLoop` has started consuming `StreamTransport` instead of relying only on `ConnectionType` checks for poller-owned stream registration.

## Not Finished Yet

These are the main reasons the overall refactor cannot be called "complete":

### 1. Plugin system is only partially migrated

Current plugin interfaces in `core/core/include/plugin` have started moving to the new model:

- `Plugin`
- `PluginManager`
- `PluginSymbolSolver`
- `PluginContext`

Gaps:

- plugin hosting has entered `core/app`, and plugin initialization now has a dedicated context object
- plugin context is improved and now includes a first plugin-facing host service catalog, but it is still not yet a full stable SDK boundary
- there is still no richer typed service/plugin registration contract aligned with the new architecture
- event bus is present, but not yet a full repository-wide extension model

### 2. Core layering is only partially converged

The long-term target architecture proposes clearer splits such as:

- runtime
- concurrency
- support
- plugin
- event bus

Those boundaries are documented, but not fully materialized in code structure yet.

Coroutine-related behavior is becoming explicit in the runtime, and shared `Task`, runtime view, plus first-stage event-loop/timeout/queue/connection awaitables now exist. But there is still no general coroutine scheduler or a fully developed repository-wide awaitable set.
There is now more than one real consumer of that shared coroutine layer: Redis and HTTP client both exercise it, which makes the coroutine mainline meaningfully more credible than when it was Redis-only.
That set now also includes DNS client, which is important because it exercises the shared coroutine layer against a datagram-style request/response path rather than only stream-oriented flows.

### 3. Protocol internals are not fully decomposed

Even though startup paths were migrated, large internal objects still exist, especially:

- `HttpServer`
- other protocol-side server/session objects
- `BitTorrentClient` and related BitTorrent runtime components still largely manage their own lifecycle internally and have not yet been deeply converged onto the shared coroutine/event/app runtime style.

So the mainline is migrated, but deep internal modularization is still in progress.

### 4. Plugin and event extensibility target is not landed

The original target included:

- plugin-based extension
- custom event support
- future coroutine-friendly evolution
- optional multi-process model

These are still target states, not finished implementation states.

### 5. Cross-platform validation is still build-oriented

The project currently builds successfully on MinGW.

But "Linux/macOS compatible" is still based on code-path intent and portability-oriented implementation choices. It has not yet been fully validated by running complete builds and runtime verification on Linux and macOS in this round.

## Practical Completion Judgment

Use the following judgment:

- If the question is "Has the mainline project refactor backbone been completed?" then yes.
- If the question is "Has the whole original end-state refactor been fully completed?" then no.

So the correct project status is:

`Mainline backbone complete, overall refactor incomplete.`

## Focused Progress Check

### Coroutine mainline

Current judgment:

- shared coroutine foundations are real
- multiple real business chains now use them
- repository-wide coroutine runtime is still incomplete

What is concretely finished:

- shared `Task<T>` exists in `Core`
- shared `RuntimeView` exists in `Core`
- a first shared scheduler foundation now exists in `Core`
- a first scheduler-native awaitable pair now exists in `Core`
- a first completion-event + sync-wait transition boundary now exists in `Core`
- `Task<T>` has now moved to lazy-start semantics
- shared event-loop / timeout / queue-in-loop / connection-event awaitables exist
- Redis uses the shared coroutine layer
- HTTP client uses the shared coroutine layer
- DNS client uses the shared coroutine layer

What is not finished:

- no fully adopted general scheduler yet across awaitables and protocol call paths
- existing Redis / HTTP client / DNS client call paths still mostly use the older bridge-style awaitables that drive `loop()` inline
- HTTP client, DNS client, Redis connect, Redis command wait, Redis subscription wait, and BitTorrent UDP tracker sync/handler announce now all contain partial migration to the new scheduler path, and BitTorrent client startup has also reduced one more business-level loop-hop. The repository still contains mixed old/new coroutine styles overall.
- no broad repository-wide awaitable set for more protocols and server-side flows
- no single unified coroutine programming model across all major subsystems
- BitTorrent and the server-side protocol paths are not yet deeply converged onto that coroutine layer

Practical judgment:

- coroutine mainline is partially complete and meaningfully usable
- coroutine end-state target is not complete

### Multi-thread / multi-process switching

Current judgment:

- run-mode metadata is present
- full runtime switching is not complete

What is concretely finished:

- `RuntimeContext` carries `RunMode` and `worker_threads`
- app/service/plugin events already propagate `run_mode` and `worker_threads`
- plugin context also carries the translated run mode
- some protocol internals, such as HTTP, already use a thread pool locally
- `Application` now normalizes run-mode context and distinguishes `single_thread` vs `multi_thread` startup behavior
- `multi_thread` now has a minimal real app-level parallel service-start strategy instead of being metadata only
- `Bootstrap` now explicitly rejects `multi_process` instead of silently pretending to support it
- `RuntimePlan` now codifies the current reactor execution mapping:
  - `single_thread -> reactor_per_service`
  - `multi_thread -> parallel_service_reactors`
  - `multi_process -> process_reactors`
- `Bootstrap` now evaluates the derived runtime plan instead of keeping the mode switch fully hardcoded
- coroutine compatibility is now explicitly judged as:
  - `single_thread`: compatible
  - `multi_thread`: compatible in the current service-owned reactor model
  - `multi_process`: compatible inside isolated worker processes because each worker still owns a local reactor runtime
- a minimal POSIX multi-process bootstrap skeleton now exists:
  - parent process acts as supervisor
  - child processes start app services locally
  - the current shape is `service-per-process`, not same-listener prefork workers
  - `RuntimeContext` now carries `worker_index` and `is_worker_process`
  - worker identity now flows into app/plugin/service runtime events and plugin host context
  - worker lifecycle events now enter the app event stream (`started / exited / restarted / restart_limit_reached`)
  - supervisor state changes now also enter the app event stream
  - supervisor now supports worker polling/reaping and two-phase `TERM -> KILL` shutdown
  - worker-local startup now reuses the `Application` lifecycle instead of permanently bypassing it
  - supervisor now enters a minimal fail-fast path when a worker exits abnormally
  - supervisor now also has a minimal bounded restart policy for failed workers
  - failed workers are now restarted through a scheduled recovery path instead of sleeping inside the reap loop
  - bounded restart now also tracks per-window restart attempts
  - when a worker reaches the per-window restart limit, supervisor now suppresses restart until the recovery window resets instead of always treating that point as terminal failure
  - a first supervisor state model now exists (`idle / starting / running / recovering / degraded / stopping / stopped`)
  - `Bootstrap` now exposes a structured `SupervisorSnapshot` instead of requiring every caller to recompute aggregate worker state
  - supervisor snapshot and state events now also expose both `recovering_workers` and `suppressed_workers`, so delayed restart backoff and restart-window suppression are visible separately to entrypoints and plugins
  - supervisor state and snapshot now also carry a structured reason code (plus its string form), and successful restart / no-recovery-window limit-hit are now distinguished as separate reasons
  - key entrypoints now print role / supervisor-state / worker identity / supervisor snapshot for runtime inspection
  - Windows/MinGW still remains unsupported for this mode

What is not finished:

- there is still no repository-level worker-runtime orchestration for `RunMode::multi_thread`
- there is still no fully robust process supervisor / restart manager for `RunMode::multi_process`
- the current multi-process shape is still `service-per-process`, not prefork same-port workers
- the current restart policy is still intentionally minimal even though it now has delayed recovery, restart suppression, backoff, and a bounded restart window
- the current network layer does not yet provide prefork-ready same-listener foundations such as `SO_REUSEPORT`, shared listener injection, or fork-safe listener re-registration
- service startup is still mostly per-service thread ownership, not a unified app-level execution model

Practical judgment:

- `multi_thread` has started to become a real app-level mode, but it is still early and incomplete
- `multi_process` has a first POSIX host skeleton, but it is still far from a completed runtime mode
- in the current repository, reactor should remain the event-loop model
- the correct near-term execution stance is "service-owned reactors with app-level startup policy", not "one global loop for every subsystem"

## Current Practical State

As of the latest verified build:

- the new app/service/plugin/event mainline is real and buildable
- the default `test_plugin` executable path now resolves the built plugin directory correctly in the MinGW build tree, and the HelloWorld plugin has been runtime-verified without requiring an explicit plugin-path argument
- the HTTP path has been repaired back to a maintainable state instead of a damaged transitional state
- the HTTP client path now has a real coroutine-facing entrypoint instead of being callback-only
- the DNS client path now has a real coroutine-facing entrypoint instead of being callback-only
- the BitTorrent path now has a service assembly wrapper, but it should still be considered early in migration compared with the other major subsystems
- the DNS packet parsing/serialization path has now been migrated onto `ByteBuffer`, making DNS the first real protocol parser chain on the new buffer mainline
- first-stage acceptor-role boundaries now exist:
  - `StreamListener`
  - `DatagramEndpoint`
- `TcpAcceptor / UdpAcceptor` now implement those role interfaces
- `UdpInstance` has started depending on `DatagramEndpoint` instead of a concrete `UdpAcceptor`
- `App`, `DnsClient`, `DnsServer`, and `PeerListener` have started consuming the new listener/endpoint role interfaces in their startup path
- a second acceptor-role layer now also exists:
  - `StreamAcceptor`
  - `DatagramAcceptor`
- `TcpAcceptor / UdpAcceptor` now implement those combined lifecycle + role interfaces
- `App`, `DnsClient`, `DnsServer`, `WebSocketServer`, `FTP server`, `PeerListener`, `UdpTracker`, and `UtpManager` startup paths have started consuming `StreamAcceptor / DatagramAcceptor` instead of wiring everything against concrete acceptor types
- `TcpConnector`, `RedisClient`, and `HttpClient` have started consuming `StreamTransport` in real runtime paths
- `UtpManager` and `DhtNode` now bind their UDP runtime through `DatagramEndpoint::endpoint_channel()` rather than only concrete `UdpAcceptor`
- `ConnectionType / get_conn_type()` have been removed from the main `Connection` abstraction, reducing one of the old TCP/UDP fake-unification leftovers
- `forward()` has been removed from the main `Connection` abstraction as an unused legacy hook
- several HTTP / FTP / Redis / KCP write paths now use narrower output helpers on `Connection` instead of directly reaching for the whole output linked-buffer chain
- `get_output_linked_buffer()` has also been removed from the main `Connection` abstraction after the remaining real write paths were migrated away from it
- `Connection::get_channel()` has now also been removed from the main `Connection` abstraction, so stream-side channel access now flows through `StreamTransport`
- `Acceptor::get_channel()` has now also been removed from the main `Acceptor` abstraction, so listener/datagram channel access now flows through `StreamListener / DatagramEndpoint`
- the old `get_input_buff(bool)` ownership-style API has been replaced by explicit `input_buffer()` and `take_input_buffer()` semantics
- `DnsServer` and BitTorrent's `UdpTracker` now consume `Connection::get_input_byte_buffer()` directly on inbound parse paths, reducing more protocol-local legacy bridge code
- bridge helpers have now been consolidated into a single `buffer_bridge.h`, and the redundant `legacy_buffer_bridge.h` header has been removed
- `ByteBuffer` now directly supports copy-style construction from raw bytes and `string_view`, and BitTorrent peer write paths have started using that directly instead of protocol-local write-buffer helper wrappers
- HTTP request/response header emission and FTP server command responses no longer depend on `Connection::current_output_buffer()` from protocol-side code
- FTP file-stream send/receive paths now also have `ByteBuffer`-based file IO helpers, and `current_output_buffer()` has been reduced to an internal `Connection` helper instead of remaining a protocol-facing API
- FTP command/response parser chains now store and parse `ByteBuffer` instead of legacy pooled `Buffer*`, and FTP client/server session parser paths no longer depend on `take_input_buffer()`
- WebSocket packed output frames now use `ByteBuffer` chunks instead of pooled legacy `Buffer*`
- WebSocket receive accumulation and frame payload delivery now run on `ByteBuffer`, and websocket handler/data-handler payload APIs have been switched from legacy `Buffer*` to `ByteBuffer`
- `core/app::App` has been detached from legacy `buffer::Buffer&` packet callbacks; the base app read path now forwards `ByteBuffer`
- `Connection` input storage now uses `ByteBuffer`, and the old public `input_buffer()/take_input_buffer()` path has been removed
- `Connection` output storage now uses `BufferChain`, and TCP/UDP runtime output queues no longer use `LinkedBuffer` as their main model
- HTTP parser chains now consume `ByteBuffer` directly, and `HttpPacket` has moved off the legacy `get_buff()/swap_buffer()` storage hooks
- Redis command parsing now uses `ByteBufferReader`, and the legacy `BufferReader` header has been removed
- Redis RESP parsing now rolls back the reader on incomplete frames, validates bulk-string trailing CRLF before committing, and parses RESP map frames by declared pair count instead of draining all remaining bytes
- Redis connection initialization now avoids running synchronous `auth/select` commands inside the `on_connected` event callback, and the timer path no longer issues synchronous `ping` commands from inside a timer callback
- Redis connection establishment now waits through the shared completion-event timeout path, and Redis command construction now flows through a shared internal `cmd_builder` helper across the real `cmd_impl` command families; the remaining direct `set_args` calls are special command-object resets for transaction/subscription state
- `DefaultCmd` now stores packed command arguments as strings internally, so `pack()` no longer has to virtual-dispatch every argument through `RedisValue::to_string()` or allocate a second serialized-argument vector
- the legacy `Buffer` header itself has also been deleted after its last real runtime include was removed
- `linked_buffer.h/.cpp` have been deleted after the last real runtime use was removed
- `buffer_bridge.h` and `BufferedPool` have also been removed after their remaining bridge/pool call sites were migrated away
- plugin configuration access now goes through `PluginConfigView`, so plugins no longer need to depend on a raw host-managed JSON pointer for basic config reads
- plugin event and logging access now go through plugin-facing `HostEventBus` and `HostLogger` facades, so `PluginContext` no longer exposes raw host `EventBus` / `LogRegistry` implementation pointers to plugin SDK consumers
- supervisor recovery now also has a process-level circuit breaker, so repeated abnormal worker exits inside a bounded window can temporarily suppress all pending restarts before recovery resumes
- `base::time` has been upgraded into a shared controllable time gateway, with steady/system clock APIs plus test override/advance hooks
- supervisor, KCP, uTP, DHT, match timing, logger timestamps, multipart temp naming, and Redis seed/random timing now route through that shared time module instead of scattered raw `std::chrono` / `time(nullptr)` calls
- socket/address primitives have been hardened around hostname resolution and non-blocking connect semantics, and endian helpers have been simplified into a clearer byte-swap based implementation
- concrete transport/acceptor construction has started moving behind core factories:
  `create_stream_acceptor / create_datagram_acceptor / create_stream_connection / create_datagram_connection`
- DNS, HTTP client/proxy, Redis, FTP, WebSocket, and BitTorrent tracker startup/runtime paths have started consuming those factories instead of directly constructing `TcpAcceptor/UdpAcceptor/TcpConnection`
- `core/app`, BitTorrent peer listener, uTP manager, and DHT node startup paths now also create acceptors through `create_stream_acceptor / create_datagram_acceptor`; the remaining direct concrete acceptor subclass is FTP passive-data transfer's specialized `PassiveTcpAcceptor`
- UDP datagram runtime binding no longer leaks `UdpConnection` into DNS client and BitTorrent tracker setup code; that attach/state path now lives on `DatagramTransport`
- `BitTorrentClient` has also started shrinking into a real orchestrator:
  tracker/peer session assembly now flows through `TrackerSessionConfig` and `PeerSessionConfig`
- `BitTorrentClient` now also routes transfer/progress/accounting state through a dedicated `DownloadStatsTracker`, so tracker announce context and periodic stats emission are no longer mixed directly into the client object
- `BitTorrentClient` now also routes NAT / tracker / peer runtime startup-stop wiring through a dedicated `DownloadRuntimeCoordinator`, reducing one more block of mixed orchestration logic inside the client object
- `BitTorrentClient` now also routes piece ownership / download-state bookkeeping through a dedicated `PieceDownloadState`, so piece-have / piece-in-flight tracking is no longer kept as raw vectors directly on the client object
- `BitTorrentClient` now has a minimal piece-completion hook: after writing a piece block it can detect full piece length, verify the piece hash, mark the piece completed, update stats, and broadcast `have` through the runtime coordinator
- BitTorrent request selection now lives in `PieceDownloadState` instead of `PeerConnection`, so peer transport no longer owns piece scheduling decisions; the current policy now supports a small per-peer request window over sequential block requests, duplicate in-flight suppression, a lightweight rarest-first preference, and requeueing of pending blocks lost with a failed peer, but still does not implement full production-grade multi-peer scheduling
- `PieceStorage` now has a minimal final-file commit path: verified piece data can be written back into the target single-file or multi-file output layout before the partial piece is removed
- BitTorrent upload now has a minimal `request -> piece` path: peer request messages can be served from completed local pieces through `PieceStorage`, though full seeding policy is still not implemented
- BitTorrent upload accounting now also flows back into `DownloadStatsTracker`, so tracker announce context can reflect uploaded bytes instead of remaining download-only
- `BitTorrentClient` startup now preloads already-committed local pieces from final files, so resume/seeding state is no longer limited only to in-session partial-piece state
- `BitTorrentClient` startup now also restores full verified `.partial.*` piece files into committed output before normal runtime begins, which hardens resume behavior for the "piece fully written but not yet committed" interruption case
- BitTorrent tracker announce flow now also carries lifecycle-aware started/completed/stopped events instead of always sending a started-style announce
- BitTorrent HTTP tracker requests now run through project `HttpClient` instead of keeping a separate raw-socket HTTP path inside the tracker module
- `BitTorrentProto` now also has a real local downloader/seeder e2e path in the test target:
  one local tracker, one local seeder, one local downloader, final-file verification, and clean stop/shutdown behavior now execute through `test_bit_torrent`
- `BitTorrentProto` is still not a complete runtime-ready downloader/uploader:
  full multi-peer block scheduling/resume policy is still incomplete, full upload/seeding policy is not implemented, uTP peer-protocol integration is incomplete, and broader real-world torrent validation is still missing
  - the repository can be treated as "mainline refactor complete"
- the repository still should not be treated as "final whole-project refactor complete"

### Mainline Round Two (proxy/coroutine hardening)

Latest verified round-two outcomes:

- Proxy integration accept path regression has been repaired; integration logs now show normal accept flow (`accepted=1`) instead of stalled listener loops.
- Event-loop poll event generation validation has been hardened to avoid discarding `generation=0` events.
- High-frequency connection-facing coroutine/context paths now use explicit connection ownership semantics (`ConnectionRef`) rather than ad hoc pointer extraction helpers.
- `make_non_owning_handler` and related session/async call sites have moved toward safer reference/shared ownership style while preserving compatibility entry points.
- Manual `new/delete` remnants in key session/async/connect paths have been reduced in favor of RAII (`std::unique_ptr` handoff to existing factory ownership).
- Key build and regression targets for this track are green:
  - `Core`
  - `App`
  - `ServerServices`
  - `test_proxy_service_integration`

Known caveat retained in current baseline:

- Windows large CONNECT behavior is now split conceptually into two cases: a strict no-half-close large tunnel path, which passes with full payload equality, and a half-close smoke path, which is timing-sensitive at the socket/read boundary and is validated as a substantial correct prefix rather than a strict full-echo guarantee.

The remaining work is no longer about recovering broken migration steps. It is about finishing the intended end-state:

- continue decomposing large protocol internals
- harden plugin/event extension boundaries into a stable SDK-level contract
- validate Linux and macOS builds beyond MinGW-only verification
- continue the BitTorrent migration beyond the new service wrapper so its internal lifecycle and runtime model converge with the rest of the repository instead of remaining mostly library-style

## Recommended Next Phase

The next work should return to the mainline instead of staying inside HTTP details.

Recommended priority:

1. Define a stable plugin/app context boundary
2. Introduce the event-bus style extension path
3. Continue shrinking protocol-side monoliths after the architecture boundary is stable
4. Validate Linux and macOS builds in addition to MinGW

## Focused Next Track

One follow-up track now has its own dedicated plan:

- `docs/NETWORK_RUNTIME_REFACTOR_PLAN.md`

That document should be treated as the focused continuation plan for:

- reducing raw network/runtime detail leakage into protocol code
- making the awaitable/scheduler family more complete
- extracting shared client/runtime facades for datagram and stream workflows
- moving service wrappers toward a clearer runtime-host boundary

Current blocker to watch closely:

- `HttpClient::connect_async(...)`

That path is the current pressure point for continuing the stream-side coroutine migration safely on MinGW/Windows.

## Exit Criteria For "Refactor Complete"

The refactor can be called complete when all of the following are true:

- `core/app` owns service lifecycle consistently
- plugins are integrated into the new lifecycle and extension model
- protocol modules no longer carry startup/composition responsibility
- major large protocol objects are decomposed into stable components
- project builds cleanly for the intended primary toolchain and target platforms
- the design target in the architecture and roadmap docs is reflected by the real code structure, not only by documents
