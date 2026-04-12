# 项目整体重构需求

## 重构目标

- 重构为现代 C++ 模式
- 支持 C++20 标准
- 支持协程
- 支持异步
- 提高性能和可维护性
- 支持插件扩展，包括协议、子模块等
- 支持自定义事件
- 支持跨平台
- 支持多进程和多线程模式可选
- 后续的 app 需要结合 `core/app` 落地完成

## 平台与编译约束

- 当前主编译链以 `MinGW` 为准
- 目标兼容平台为 `Linux` 和 `macOS`
- 本轮整体重构默认不以 `MSVC` 兼容为优先目标

## 本轮落地产物

- 项目级重构设计文档：
  [docs/PROJECT_REFACTOR_ARCHITECTURE.md](docs/PROJECT_REFACTOR_ARCHITECTURE.md)
- 项目级演进路线文档：
  [docs/PROJECT_REFACTOR_ROADMAP.md](docs/PROJECT_REFACTOR_ROADMAP.md)
- 顶层 CMake 重构：
  统一项目开关、构建入口和模块装配策略
- `core/app` 最小装配层：
  `Application / Bootstrap / RuntimeContext / Service`
- `main.cpp` 示例入口：
  已改为基于 `core/app` 的服务装配方式
- `server/match` 服务化改造：
  已通过 `MatchService` 接入 `core/app`
- `protocol/http` 服务化改造：
  已通过 `HttpService` 接入 `core/app`
- `test_http_server` 入口改造：
  已改为基于 `Application + Bootstrap + HttpService`
- `protocol/websocket` 服务化改造：
  已通过 `WebSocketService` 接入 `core/app`
- `test_websocket_server` 入口改造：
  已改为基于 `Application + Bootstrap + WebSocketService`
- `protocol/ftp` 服务化改造：
  已通过 `FtpService` 接入 `core/app`
- `test_ftp_server` 入口改造：
  已改为基于 `Application + Bootstrap + FtpService`
- `server/services` 新增：
  统一承载 `DnsService / HttpService / WebSocketService / FtpService`
- 协议层依赖收口：
  `HttpProto / FtpProto / WebSocketProto` 已去掉对 `App` 的直接依赖

## 本轮重构范围说明

这次不是一次性重写所有模块，而是先完成“整体重构的骨架层”：

- 明确新的目标分层和模块边界
- 明确 `core / protocol / libs / plugins / server / test / app` 的职责
- 统一构建开关，支持按模块渐进演进
- 为后续协程化、异步化、插件化和多进程化保留稳定扩展点

后续按照演进文档分阶段推进代码迁移，而不是在一个阶段内同时推倒所有协议实现。

## 当前进展

已完成：

- 顶层构建骨架收口
- 项目级目标架构与演进文档
- `core/app` 最小应用装配框架
- `main.cpp` 服务化示例入口
- `server/match` 从独立散装入口迁移到 `core/app`
- `protocol/http` 新增服务适配层
- `test_http_server` 从直接驱动协议对象迁移到 app 装配入口
- `protocol/websocket` 新增服务适配层
- `test_websocket_server` 从直接驱动协议对象迁移到 app 装配入口
- `protocol/ftp` 新增服务适配层
- `test_ftp_server` 从直接驱动协议对象迁移到 app 装配入口
- 服务适配层从 `protocol/*` 抽离到 `server/services`
- 协议层重新收口为“协议实现”，服务层负责装配与启动
- `HttpServer` 已开始做内部生命周期拆分：
  运行时初始化、事件循环绑定、资源清理已从主流程中抽离为独立方法
- `HttpServer` 的 HTTP 特性初始化继续拆分：
  内建路由注册与代理初始化已从通用初始化流程中独立出来
- `HttpServer::on_read()` 已开始拆分：
  请求解析、请求分发、请求收尾已抽成独立私有方法，主流程开始收口
- `HttpServer::serve_upload()` 已完成阶段拆分：
  请求参数校验、上传会话定位、分片持久化、完成收尾已拆成独立私有方法
- `HttpServer::serve_static()` 已完成阶段拆分：
  静态路径解析、内建页面响应、文件流式响应已拆成独立私有方法
- `HttpServer` 会话收尾链已开始收口：
  会话查找、连接输入缓冲清理、会话释放开始通过 `.cpp` 内部 helper 复用，减少 `on_read/finalize_request/free_session` 内联重复逻辑
- `PluginManager` 已去掉旧消息式插件加载职责：
  不再承担 `system_message` 的插件装载/释放消费，职责收口为动态库加载、实例创建、初始化与释放
- 插件主线已去掉 `MessageDispatcher` 依赖：
  `PluginContext` 不再注入旧 dispatcher，`Plugin/HelloWorld` 也不再继承旧消息 consumer 模型，旧消息分发实现已从主线构建中移除
- `PluginContext` 已补充插件级身份信息：
  插件初始化阶段可直接获得 `plugin_name / plugin_root_path`，减少对外部推断和硬编码路径的依赖
- `PluginContext` 已补充宿主日志入口：
  插件可通过宿主提供的 `log_registry` 接入统一日志基础设施，而不必自行抓取全局日志单例
- `PluginContext` 已补充插件配置定位入口：
  插件初始化阶段可直接获得 `plugin_config_path`，后续可在此边界上继续叠加统一配置访问器
- `PluginContext` 已补充最小配置访问能力：
  若存在插件同名 JSON 配置文件，宿主会在初始化前完成解析并通过 `plugin_config` 提供给插件
- `PluginContext` 已开始补充宿主服务发现能力：
  `RuntimeContext` 新增 `ServiceRegistry`，并通过插件侧 `HostServiceCatalog` 适配器把宿主服务发现能力暴露给插件，避免在插件 SDK 里直接泄漏 `core/app` 内部类型
- 协程实现状态已确认：
  当前仓库在 `redis_cli` 与 `rpc` 辅助层存在局部 C++20 coroutine helper，但尚未形成仓库级统一 scheduler / awaitable / coroutine runtime
- 仓库级最小协程基础层已落地：
  `Core` 中新增统一 `Task<T>`，`redis_cli/rpc` 现有 helper 已收敛为基于该公共实现的兼容别名
- 仓库级最小 awaitable 已落地：
  `Core` 中新增基于 `EventLoop` 的统一 awaitable，Redis 真实协程调用链已从局部 `suspend_always + loop()` 切到公共桥接实现
- 仓库级 timeout / queue-in-loop awaitable 已落地：
  `Core` 中已补充超时等待与 `queue_in_loop` 桥接 awaitable，Redis 获取消息超时链已切到公共 timeout awaitable
- 仓库级 coroutine runtime view 已落地：
  `Core` 中新增统一 `RuntimeView`，Redis 协程调用链已开始通过该视图收口 `EventLoop/TimerManager` 的协程侧访问
- 仓库级 connection-event awaitable 已落地：
  `Core` 中新增连接事件桥接 awaitable，Redis 连接建立等待链已开始接入该公共实现
- 协程恢复动作已开始通过 runtime view 收口：
  Redis 事件回调里的恢复逻辑开始经由 `RuntimeView::request_resume()`，减少对 `RedisRegistry` 手动恢复入口的直接耦合

下一步建议：

- 开始收口测试目录与 examples/server 目录的职责边界
- 继续拆分协议内部的“大对象”，例如 `HttpServer`
## 2026-04 coroutine mainline update

- `http_client` now has a coroutine request API built on shared coroutine runtime primitives.
- `dns_client` now has a coroutine query API built on shared coroutine runtime primitives.
- `DnsClient` has started internal runtime lifecycle decomposition.
- `DnsServer` has started internal runtime lifecycle decomposition.
- `ByteBuffer / BufferChain` now exist as the new buffer mainline in `Core`.
- DNS packet encode/decode has been migrated onto `ByteBuffer`.
- DNS client/server now keep legacy `Buffer` only at the transport boundary while DNS protocol serialization/parsing runs on `ByteBuffer`.
- `BitTorrentProto` now has a first service-layer wrapper in `server/services`.
- `BitTorrentClient` has started internal runtime lifecycle decomposition.
- `BitTorrentClient` startup flow has started deeper decomposition around NAT/stats setup.
- `BitTorrentClient` now splits tracker announce coordination into a `TrackerSession` helper and peer connection lifecycle management into a `PeerSession` helper, leaving the client closer to orchestration + download-state ownership.
- `PeerConnection` now feeds close/error state changes back into session-level cleanup so peer lifetime management can stay centralized.
- `BitTorrent` UDP tracker request/response packing has been migrated onto `ByteBuffer`.
- `BitTorrent` peer handshake/message accumulation has started moving onto `ByteBuffer` in `PeerConnection / PeerListener`.
- `BitTorrent` uTP and DHT UDP send paths now use `ByteBuffer` before the final legacy transport adaptation.
- `StreamTransport / DatagramTransport` interfaces now exist as first-stage transport boundaries.
- `TcpConnection / UdpConnection` now implement those transport-role interfaces, and `EventLoop` has started using `StreamTransport` instead of only `ConnectionType` checks for stream registration.
- `StreamListener / DatagramEndpoint` interfaces now exist as first-stage acceptor-role boundaries.
- `TcpAcceptor / UdpAcceptor` now implement those listener/endpoint role interfaces.
- `UdpInstance` now depends on `DatagramEndpoint` instead of directly depending on `UdpAcceptor`.
- `BitTorrent` uTP and DHT send paths now consume `DatagramEndpoint`, not only concrete `UdpAcceptor`.
- `App`, `DnsClient`, `DnsServer`, and `PeerListener` have started consuming `StreamListener / DatagramEndpoint` in their startup path instead of only calling legacy `get_channel()` on concrete acceptors.
- `TcpConnector`, `RedisClient`, and `HttpClient` have started consuming `StreamTransport` instead of only assuming `Connection::get_channel()` on stream connections.
- `UtpManager` and `DhtNode` now update the event loop through `DatagramEndpoint::endpoint_channel()` instead of concrete `UdpAcceptor::get_channel()`.
- `StreamAcceptor / DatagramAcceptor` now exist as combined lifecycle + role boundaries above `StreamListener / DatagramEndpoint`.
- `TcpAcceptor / UdpAcceptor` now implement those combined acceptor-role interfaces.
- `App`, `DnsClient`, `DnsServer`, `WebSocketServer`, `FTP server`, `PeerListener`, `UdpTracker`, and `UtpManager` startup paths have started consuming `StreamAcceptor / DatagramAcceptor` instead of hard-coding concrete acceptor types in their main wiring path.
- `ConnectionType / get_conn_type()` have now been removed from the main connection abstraction; stream/datagram role checks are now expected to flow through transport-role interfaces instead of enum tagging.
- `forward()` has now been removed from the main connection abstraction because it was no longer used by real runtime paths.
- `Connection` now exposes narrower output helpers (`current_output_buffer / append_output`) so protocol code no longer has to grab the entire `LinkedBuffer` in several mainline write paths.
- `get_output_linked_buffer()` has now also been removed from the main connection abstraction after the remaining mainline write paths were narrowed onto `current_output_buffer / append_output`.
- `Connection::get_channel()` has now been removed from the main connection abstraction; stream-side callers are expected to depend on `StreamTransport`, and listener-side callers are expected to depend on `StreamListener`.
- `Acceptor::get_channel()` has also been removed from the main acceptor abstraction; listener/datagram runtime wiring now flows through `StreamListener / DatagramEndpoint`.
- `Connection::get_input_buff(bool)` has now been replaced by explicit `input_buffer() / take_input_buffer()` semantics, reducing one more ambiguous ownership-style API from the main transport abstraction.
- `DnsServer` and `BitTorrent`'s `UdpTracker` have now moved more of their inbound parsing path onto `Connection::get_input_byte_buffer()`, reducing legacy `Buffer -> ByteBuffer` bridge code in protocol handlers.
- bridge helpers have now been consolidated into a single `buffer_bridge.h`, and the redundant `legacy_buffer_bridge.h` compatibility header has been deleted.
- `ByteBuffer` now directly supports copy-style construction from raw bytes / `string_view`, and BitTorrent peer write paths have started using it directly instead of protocol-local `make_write_buffer(...)` helpers.
- HTTP request/response header emission and FTP server command responses no longer depend on `Connection::current_output_buffer()` from protocol-side code.
- FTP file-stream send/receive paths now use `ByteBuffer`-based file IO helpers, and `current_output_buffer()` has been reduced to an internal `Connection` helper instead of a protocol-facing API.
- FTP command/response parser chains now store and parse `ByteBuffer` instead of legacy pooled `Buffer*`, and FTP client/server sessions no longer depend on `take_input_buffer()` in their main parser path.
- WebSocket send path now uses `ByteBuffer` for packed output frames and no longer depends on pooled `Buffer*` output chunks.
- WebSocket receive path now accumulates and parses into `ByteBuffer`, and websocket handler/data-handler payloads now use `ByteBuffer` instead of legacy `Buffer*`.
- `core/app::App` no longer exposes legacy `buffer::Buffer&` as its packet callback surface; the base app read path now forwards `ByteBuffer`.
- `Connection` input storage has now moved onto `ByteBuffer`, and the old `input_buffer()/take_input_buffer()` path has been removed from the public connection abstraction.
- `Connection` output storage has now moved onto `BufferChain`, and TCP/UDP write queues no longer use `LinkedBuffer` as their main runtime model.
- HTTP parser chains now consume `ByteBuffer` directly, and `HttpPacket` no longer relies on legacy `get_buff()/swap_buffer()` packet storage hooks.
- Redis command parsing has now moved from the old `BufferReader` onto `ByteBufferReader`, and the legacy `BufferReader` header has been removed.
- Redis RESP parsing now rolls back `ByteBufferReader` on incomplete frames, validates bulk-string trailing CRLF before commit, and parses map frames by declared pair count.
- Redis connection initialization no longer runs synchronous `auth/select` commands inside the `on_connected` callback, and the timer path no longer issues synchronous `ping` from inside timer callbacks.
- Redis connection establishment now waits through the shared completion-event timeout path, and real Redis `cmd_impl` command construction now flows through a shared internal `cmd_builder` helper; the remaining direct `set_args` calls are special transaction/subscription command-object resets.
- `DefaultCmd` now stores command arguments as strings internally, so packing avoids per-argument virtual dispatch and the extra serialized-argument vector.
- the legacy `Buffer` header itself has now been deleted after the last real include was removed from runtime code.
- The last real `LinkedBuffer` usage has been eliminated from runtime code, and `linked_buffer.h/.cpp` have now been deleted.
- The old `buffer_bridge.h` compatibility layer and `BufferedPool` implementation have also been removed after their remaining call sites were migrated away.
- plugin configuration access has now been wrapped behind `PluginConfigView`, so plugins no longer need to depend on a raw host-owned JSON pointer to read config values.
- plugin event and logging access now go through plugin-facing `HostEventBus` and `HostLogger` facades, so `PluginContext` no longer exposes raw host `EventBus` / `LogRegistry` implementation pointers to plugin SDK consumers.
- Coroutine status: usable mainline exists, but repository-wide coroutine end-state is not complete.
- RunMode status: `single_thread / multi_thread / multi_process` metadata exists, but real app-level switchable runtime modes are not complete.
- `Application/Bootstrap` now has a minimal real `multi_thread` startup path; `multi_process` is still not implemented.
- `RuntimePlan` now codifies reactor execution strategy:
  - `single_thread -> reactor_per_service`
  - `multi_thread -> parallel_service_reactors`
  - `multi_process -> process_reactors`
- Coroutine/runtime compatibility judgment is now explicit:
  - `single_thread`: compatible with current `RuntimeView + EventLoop + TimerManager`
  - `multi_thread`: compatible in the current service-owned reactor model
  - `multi_process`: coroutine runtime can run inside isolated worker processes; cross-process host contract is still incomplete
- Current reactor stance:
  keep reactor mode, keep event loops service-owned, and let `Application` control startup policy instead of forcing a single global loop.
- `Bootstrap` now has a first POSIX multi-process skeleton:
  parent acts as supervisor, child workers start app services locally, Windows/MinGW still does not support this mode.
- Worker-local runtime identity is now wired through runtime context, app events, server service events, plugin events, and plugin host context.
- POSIX supervisor lifecycle is no longer fire-and-forget only:
  worker polling/reaping and a minimal `TERM -> KILL` shutdown path are now in place.
- Worker-local startup is now closer to the mainline lifecycle:
  child processes reuse `Application` startup instead of permanently bypassing app-level init/start semantics.
- Supervisor now has a minimal fail-fast path:
  abnormal worker exit will trigger shutdown of remaining workers instead of silently drifting on.
- Supervisor now also has a bounded restart path for failed workers.
- Failed-worker restart now also tracks attempts inside a bounded restart window.
- Failed-worker restart is now scheduled through a recovery window instead of sleeping inline inside reap logic, and `SupervisorSnapshot` now carries both `recovering_workers` and `suppressed_workers`.
- When a worker reaches the per-window restart limit, supervisor now suppresses restart until the recovery window rolls over, instead of immediately turning every limit-hit into terminal failure.
- Supervisor state/snapshot now also carry structured reason codes and string forms, and now distinguish `worker_restarted` from `restart_limit_without_recovery_window`, so entrypoints and plugins can observe recovery transitions without scraping logs.
- Supervisor recovery now also has a process-level circuit breaker: repeated abnormal worker exits inside a bounded window can temporarily suppress all pending restarts until a supervisor recovery backoff elapses.
- Worker lifecycle has entered the app event stream:
  `worker_started / worker_exited / worker_restarted / worker_restart_limit_reached`.
- Supervisor state changes have also entered the app event stream, and `Bootstrap` now exposes a structured supervisor snapshot.
- The HelloWorld plugin now subscribes to worker and supervisor lifecycle events, so plugin-side observability is no longer limited to app/plugin load events.
- Key service entrypoints now print role / supervisor-state / worker identity / supervisor snapshot at startup.
- `test_plugin` now resolves the built plugin directory correctly by default in the MinGW build tree; HelloWorld has been runtime-verified without passing an explicit plugin path.
- Prefork boundary is now explicit:
  current network layer is not ready for same-port prefork workers because it lacks `SO_REUSEPORT`, shared listener injection, and fork-safe listener re-registration.
- A first scheduler foundation now exists in `Core`:
  `Scheduler / EventLoopScheduler / RuntimeView::schedule()` have landed, and `EventLoop` can now accept posted coroutine handles instead of only callback closures.
- `Task<T>` now has continuation support and can be `co_await`-ed directly, which starts moving coroutine composition closer to mainstream coroutine runtimes.
- `Task<void>` and `sync_wait(runtime, Task<void>)` now also exist, and the coroutine smoke test no longer depends on hand-written `queue_in_loop()` from test code.
- Legacy bridge awaitables have now been removed from the main runtime facade, and the old bridge-only event-loop awaiter header has been deleted.
- `FtpClient` now has a first wait boundary for connection/response/list-output/transfer-idle state, which starts reducing `sleep + poll` business code on the FTP client side.
- `FtpClient` now also has `wait_for_local_file(...)` for download-side caller workflows.
- FTP interactive/e2e client-side entrypoints have also started using those wait helpers.
- `test_http_client` has been migrated from callback chaining to the coroutine API.
- `test_dns_client` has been migrated from callback-style querying to the coroutine API.
- `base::time` is now the shared repository time gateway:
  steady/system clocks, test-time override/advance hooks, and legacy `now()/get_tick_count()` compatibility now all route through one module.
- key runtime paths now consume `base::time` instead of raw `std::chrono`:
  supervisor timing, KCP update timestamps, uTP retransmit timing, DHT expiry windows, match wait-time tracking, logger timestamps, multipart upload temp naming, and Redis random seeding.
- socket/address primitives have been hardened:
  `InetAddress` now uses `getaddrinfo`/`inet_pton`/`inet_ntop`, `Socket::connect()` handles non-blocking in-progress states more safely, and socket lifecycle checks no longer skip fd `0`.
- endian helpers have been rewritten to a smaller, clearer byte-swap based implementation instead of the old union/macro-heavy variant.
- concrete transport/acceptor construction has started moving behind core factories:
  `create_stream_acceptor / create_datagram_acceptor / create_stream_connection / create_datagram_connection`.
- `core/app`, BitTorrent peer listener, uTP manager, and DHT node startup paths now use those acceptor factories instead of directly constructing `TcpAcceptor / UdpAcceptor`.
- protocol/runtime call sites such as DNS, HTTP client/proxy, Redis, FTP, WebSocket, and BitTorrent tracker paths no longer directly `new TcpAcceptor/UdpAcceptor/TcpConnection` on their mainline startup path.
- UDP datagram attachment/state wiring has been lifted into `DatagramTransport`, so DNS client and BitTorrent UDP tracker no longer need to downcast to `UdpConnection` just to bind instance/state.
- `BitTorrentClient` session bootstrap has been narrowed:
  tracker/peer session wiring now flows through `TrackerSessionConfig` and `PeerSessionConfig`, reducing client-side assembly sprawl.
- `BitTorrentClient` now also routes transfer/progress/accounting state through a dedicated `DownloadStatsTracker`, reducing one more mixed-responsibility block inside the client orchestrator.
- `BitTorrentClient` now also routes NAT / tracker / peer runtime startup-stop wiring through a dedicated `DownloadRuntimeCoordinator`, reducing one more mixed-responsibility orchestration block inside the client.
- `BitTorrentClient` now also routes piece ownership / download-state bookkeeping through a dedicated `PieceDownloadState`, so piece-have / piece-in-flight tracking is no longer kept as raw vectors directly on the client.
- `BitTorrentClient` now has a minimal piece-completion hook: after writing a piece block it can detect full piece length, verify the piece hash, mark the piece completed, update stats, and broadcast `have` through the runtime coordinator.
- BitTorrent request selection now lives in `PieceDownloadState` instead of `PeerConnection`, so peer transport no longer owns piece scheduling decisions; the current policy now supports sequential per-piece block requests, but not full rarest-first / multi-peer request-window scheduling.
- `PieceStorage` now has a minimal final-file commit path: verified piece data can be written back into the target single-file or multi-file output layout before the partial piece is removed.
- `PieceStorage` and `BitTorrentClient` startup now also restore fully written, hash-valid `.partial.*` piece files into committed output, covering one practical interrupted-download resume case.
- BitTorrent upload now has a minimal `request -> piece` path: peer request messages can be served from completed local pieces through `PieceStorage`, though full seeding policy is still not implemented.
- `BitTorrentProto` now has a real local downloader/seeder e2e path in `test_bit_torrent`:
  one local tracker, one local seeder, one local downloader, final-file verification, and clean shutdown all run through the executable test target.
- `BitTorrentProto` should still be treated as incomplete for real-world download/upload:
  the current codebase has protocol building blocks and runtime structure, but full multi-peer block scheduling/resume policy, full upload/seeding policy, full uTP peer-protocol integration, and broader real-world torrent validation are still missing.
- Current judgment:
  `mainline backbone complete, overall refactor incomplete`.
