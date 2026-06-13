# Game Server 设计说明

本文说明 `libs/game/server` 当前的本地多进程 GameServer 骨架设计、服务边界、消息流、登录与会话模型、容灾策略和测试方式。

这套代码的目标不是把所有游戏逻辑写完，而是提供一个可继续生产化的服务端骨架：每个服务独立进程，公共协议与进程间消息统一，外部客户端只能进入 gateway，内部服务通过 service id 路由。

## 设计目标

- 每个服务一个独立进程，使用 `yuan::app::Application` / `yuan::app::Service` 生命周期。
- 服务启动只读 JSON 配置，不在代码里硬编码 endpoint；正式配置不允许端口 `0` 或缺必要 host。
- `libs/rpc` 保持通用 RPC 协议/抽象库，不依赖 Core 网络实现。
- GameServer 复用 Core 的网络、timer、HTTP、Redis client 等基础能力。
- 外部客户端只连接 gateway，不暴露 tunnel/world/zone/global 内部 RPC 端口。
- world 负责选区、角色轻量状态、zone freshness、登录 reservation 和 ownership fencing。
- gateway 负责客户端会话、CS frame 校验、登录态校验、转发到 zone。
- zone 负责玩家在线运行数据、准入控制、业务执行和定时持久化。
- tunnel 负责内部 service id 注册、发现、转发、广播和异步 reply。
- Redis 是外部进程，GameServer 不内嵌也不启动 Redis。

## 目录结构

```text
libs/game/server/
  common/        公共业务协议、service id、配置解析、RPC 网络适配、CS frame codec
  messaging/     通用进程间消息能力，封装 tunnel endpoint、heartbeat、failover
  tunnel/        内部服务注册与 service id 转发基础设施
  global/        全局服务示例，GM 转发/全局功能入口
  world/         选区、角色轻量状态、登录选项、ownership fencing、HTTP login_options
  zone/          玩家运行数据、登录准入、业务执行、数据 flush
  gateway/       外部客户端入口，会话表、CS frame 校验、直连 zone
  web/           外部 web/auth 方向入口，通过 world 获取游戏登录选项
  client/        本地 smoke/mock client
  config/        JSON 示例配置，构建时复制到 build 输出目录
  test/          GameServer 对应测试
```

服务内部继续按职责分层：

- `app/`：进程启动、配置、生命周期、网络/Redis/tunnel/http wiring。
- `rpc/`：内部 RPC 接口注册、decode、调用 handler、encode response。
- `handler/`：web 侧 handler 注册与实现。
- `model/`：业务对象、缓存、会话表、ownership store 等业务状态。

## 服务拓扑

```mermaid
flowchart LR
    Client[Game Client] --> Gateway[Gateway Process]
    Browser[Web/Auth Client] --> Web[Web Process]
    Web -->|HTTP /game/login_options| World[World Process]

    Gateway -->|direct RPC by zone_service_id| ZoneA[Zone Process A]
    Gateway -->|direct RPC by zone_service_id| ZoneB[Zone Process B]

    Global[Global Process] --> Tunnel[Tunnel Process]
    World --> Tunnel
    ZoneA --> Tunnel
    ZoneB --> Tunnel
    Tunnel --> Global
    Tunnel --> World
    Tunnel --> ZoneA
    Tunnel --> ZoneB

    Web --> Redis[(Redis)]
    World --> Redis
    ZoneA --> Redis
    ZoneB --> Redis
```

当前关键边界：

- `gateway` 不连接 world，不连接 tunnel，只根据配置直连 zone。
- `web` 不依赖 GameServiceId/tunnel 这些内部字段，只知道 world HTTP 地址。
- `world/global/zone` 会注册到 tunnel，首次失败不退出，后台重试。
- `zone` 注册/上报给 world 时带自身负载和对外 gateway 地址。
- 一个 gateway 可以服务多个 zone，一个 zone 当前配置约束只对应一个 gateway。

## 进程与职责

| 进程 | 主要职责 | 对外/对内关系 |
| --- | --- | --- |
| `game_tunnel_server` | 服务注册、service id 路由、随机/广播转发、异步 reply | 内部 RPC 基础设施 |
| `game_global_server` | 全局服务与 GM 示例入口 | 注册到 tunnel |
| `game_world_server` | 登录选项、选区、zone freshness、reservation、role ownership fencing、HTTP API | 注册到 tunnel，同时监听 HTTP |
| `game_zone_server` | 玩家在线状态、zone admission、业务执行、定时 flush、上报 world | 注册到 tunnel，连接 Redis |
| `game_gateway_server` | 客户端入口、CS frame/session 校验、登录态校验、转发到 zone | 对客户端暴露，对 zone direct RPC |
| `game_web_server` | 外部 web/auth 方向入口 | 连接 Redis 和 world HTTP |
| `game_mock_client` | 本地 smoke 客户端 | 模拟 web/world/gateway/zone 完整路径 |

## Service Id

GameServer 使用 packed service id 做内部路由：

```text
PackedGameServiceId = uint64_t

bits:
  region   12 bits
  world    12 bits
  type     12 bits
  instance 28 bits
```

示例：

```cpp
pack_game_service_id(1, 1, GameServiceType::zone, 2)
```

这个 id 用于：

- tunnel 注册 endpoint。
- tunnel 指定实例转发。
- world 返回 `zone_service_id` 给登录流程。
- gateway 根据 `zone_service_id` 查 direct zone endpoint。
- zone/world/gateway 做 ownership/session fencing。

## 配置模型

配置文件位于 `libs/game/server/config/*.json`，构建时由 `copy_game_server_configs` 复制到 build 输出目录。

主要配置字段：

- 通用监听：`listen_host`、`port`。
- 服务标识：`region`、`world`、`service_type`、`instance`。
- tunnel endpoint：`tunnel_endpoints`。
- gateway endpoint：`gateway_endpoints`。
- zone endpoint：`zone_endpoints`。
- world 登录 reservation：`login_reservation_ttl_ms`。
- world zone freshness：`zone_report_ttl_ms`。
- world ownership store：`world_ownership_store`，可选 `memory` 或 `redis`。
- zone 负载同步：`zone_load_sync_interval_ms`。
- zone 准入容量：`zone_max_players`。
- tunnel heartbeat：`tunnel_heartbeat_interval_ms`。
- 周期 metrics 日志：`metrics_log_interval_ms`，`0` 表示关闭。
- gateway CS frame 大小限制：`client_frame_max_bytes`。
- gateway CS frame 限流：`client_frame_max_per_window`、`client_frame_rate_window_ms`。

配置约束：

- 正式配置不接受无效 host 或端口 `0`。
- `global/world/zone` 使用 `tunnel_endpoints` 数组，可配置多个 tunnel。
- `gateway` 使用 `zone_endpoints`，不配置 tunnel/world。
- `zone` 必须配置且只能配置一个 `gateway_endpoints`，用于上报给 world。
- `web` 只需要 world HTTP 地址和自身外部配置，不需要内部 GameServiceId/tunnel 细节。

## 内部 RPC 与 tunnel 路由

内部 RPC route 使用数字协议，定义在 `common/game_rpc_protocol.h`。字符串 route name 只作为 debug/fallback 信息。

tunnel 支持三种转发模式：

- `specific`：指定一个具体 service instance。
- `random_one`：在某个 service type 下随机选一个实例。
- `all_of_type`：广播给某个 service type 下所有实例。

内部消息流：

```mermaid
sequenceDiagram
    participant Z as Zone
    participant T as Tunnel
    participant W as World
    participant G as Global

    Z->>T: tunnel.register(zone service_id, endpoint)
    W->>T: tunnel.register(world service_id, endpoint)
    G->>T: tunnel.register(global service_id, endpoint)
    Z->>T: tunnel.forward(target_service_id=world, world.player_zone_set)
    T->>W: world.player_zone_set
    W-->>T: RPC response
    T-->>Z: RPC response
```

`ProcessMessageManager` 在 `messaging/` 中提供服务侧通用能力：

- 多 tunnel endpoint 管理。
- heartbeat。
- register/forward/send/random/broadcast。
- failover 与 reconnect probe。
- retry attempts/retries/recoveries/failures 计数。

## 登录与选区流程

当前登录分两段：先从 web/world 获取登录选项，再连 gateway 登录目标 zone。

```mermaid
sequenceDiagram
    participant C as Client
    participant Wb as Web
    participant Wo as World
    participant Gw as Gateway
    participant Zn as Zone

    C->>Wb: auth/login or bootstrap
    Wb->>Wo: HTTP GET /game/login_options?player_uid=...
    Wo->>Wo: prune expired reservations
    Wo->>Wo: mark stale zones unavailable
    Wo->>Wo: select least-loaded fresh zone
    Wo->>Wo: create/refresh pending reservation
    Wo-->>Wb: roles + selected zone_service_id + gateway address
    Wb-->>C: login options

    C->>Gw: gateway.login CS frame + ClientLoginRequest
    Gw->>Gw: allocate gateway_session_id
    Gw->>Zn: zone.player_enter direct RPC
    Zn->>Zn: admission control + PlayerManager online
    Zn->>Wo: world.player_zone_set via tunnel
    Zn-->>Gw: ClientLoginResponse
    Gw-->>C: ClientLoginResponse with gateway_session_id
```

选区规则：

- world 不使用 `%` 求余选择 gateway/zone。
- 已登录角色优先保持当前 zone。
- 新登录角色从 fresh、available、未满载的 zone 中选 `online_players + pending_reservations` 最小者。
- 同一个 role 重复请求 login options 会复用并刷新同一 reservation。
- reservation 到期后会被清理。
- zone 定时上报负载；超过 `zone_report_ttl_ms` 未上报会被标记 unavailable。
- zone 自身还有 admission control，作为最终准入保护。

## Gateway 长连接、CS Frame 与会话

gateway 当前对客户端暴露的是 Core TCP 上的 YuanRpc frame stream。也就是说，网络层是长连接 framed RPC：同一 TCP 连接可以连续发送多个 YuanRpc request frame，server 按 frame 完整性解包后再调用 gateway handler。

当前 gateway 收包链路：

```text
TCP bytes
  -> RpcNetworkServer per-connection FrameStreamDecoder
  -> complete YuanRpc request frame
  -> rpc::Server route dispatch
  -> gateway handler
  -> decode CS frame from message.payload
  -> validate session/sequence/rate/header
  -> direct RPC to zone
```

`RpcNetworkServer` 的关键语义：

- 支持半包：`DecodeError::need_more` 时保留 buffer，等待后续 read。
- 支持粘包：一次 read 中有多个完整 YuanRpc frame 时循环处理。
- 支持同一 TCP 连接多次 request/response，不在每次 response 后主动 close。
- 协议错误会关闭当前连接并清理该 connection 的 decoder state。
- 每个 request 会带内部 metadata `rpc.connection_id` 进入 handler，用于绑定 gateway session 到真实 TCP connection。
- handler 可以返回内部 metadata `rpc.close_connection=1`，adapter 会先写 response 再关闭连接。
- `RpcNetworkServerConfig` 支持连接生命周期控制：`max_connections`、`max_buffered_bytes`、`idle_timeout_ms`。
- `close_all_connections()` 用于 drain 时主动关闭当前 active connections。
- `active_connection_count()` 提供结构化 metrics accessor。
- `stop_after_requests` 只用于测试里让 server run loop 退出，不代表生产连接生命周期。

当前 smoke/mock client 已验证同一个 gateway TCP 连接上连续完成 login、game、time sync、logout。内部 world/zone/tunnel 调用仍可继续使用短连接 `RpcNetworkClient::call(...)`。

如果后续要接“非 YuanRpc 包裹”的 raw CS socket，已有 `ClientFrameStreamDecoder` 可以直接复用在 socket read callback 上，实现 CS frame 自身的半包/粘包处理。

客户端到 gateway 的正式 CS 包使用 `common/client_frame.*`：

```text
magic      uint32  "CSF1"
version    uint32  1
player_uid uint64
role_id    uint64
zone_id    uint64
session_id uint64
sequence   uint64
route_svc  uint32
route_mtd  uint32
payload_len uint32
payload bytes
```

CS frame header 中的 `gateway_session_id` 是 gateway 的会话凭证。登录前为 `0`，登录成功后 gateway 分配非零 session id，后续 game/time/logout 都必须带回。

`sequence` 是客户端请求序列号：

- 登录帧允许 `sequence=0`，因为此时还没有 gateway session。
- 登录后的 framed game/time/logout 必须带非零 sequence。
- gateway 默认按 session 做 strict sequence 校验，重复 sequence、跳号、倒退都会被拒绝。
- `ClientFrameReplayGuard` 负责维护 `gateway_session_id -> last_sequence`。
- `ClientFrameValidationOptions::max_frame_bytes` 用于限制单包 payload 大小，避免解析前无限制分配。

gateway 会做两层校验：

- frame header 与 payload 中的 player/role/session/route 必须一致。
- session id 必须匹配 gateway session table 中当前 role 的登录态。
- framed post-login 请求必须通过 sequence/replay/size 校验。
- malformed request、frame header mismatch、replay/gap、invalid session 等协议违规会返回错误并请求关闭当前连接。

session table 当前保存：

```text
role_id -> zone_service_id
role_id -> gateway_session_id
gateway_session_id -> role_id / zone_service_id / connection_id
```

`connection_id` 由 `RpcNetworkServer` 注入。gateway login 成功时会记录：`gateway_session_id -> connection_id`。连接关闭后，gateway 会在后台 cleanup 线程中：

- 找到该 connection 上的所有 session。
- 向对应 zone 发送 logout。
- 清理本地 session table。
- 清理该 session 的 frame replay/rate state。

## Gateway Push 骨架

当前已经有 `gateway.push` 内部 RPC route 和 `ClientPushMessage` codec，用于承接后续 zone -> gateway -> client 的异步推送路径。

```mermaid
sequenceDiagram
    participant Z as Zone
    participant G as Gateway
    participant C as Client Connection

    Z->>G: gateway.push(role_id, gateway_session_id, payload)
    G->>G: validate role/session still active
    G-->>Z: ok / not_found / unavailable
    Note over G,C: gateway 根据 gateway_session_id 找到 connection_id 并写 YuanRpc push frame
```

当前行为：

- `gateway.push` 会 decode `ClientPushMessage`。
- gateway 校验 `role_id + gateway_session_id` 是否仍是当前登录态。
- 如果配置了 `push_handler`，会调用 handler；`GatewayServerService` 默认 handler 会把 `ClientPushMessage` 编成 YuanRpc `push` frame 写回该 session 对应的 TCP connection。
- 如果 session 不存在、role 不匹配或 connection 已断开，返回 `RpcStatus::unavailable`。

## Zone 玩家模型与持久化

zone 是玩家运行态数据的 owner。

当前模型：

- `Player`：单个玩家/角色运行对象。
- `PlayerManager`：玩家缓存、在线集合、dirty 状态、flush。
- `online_roles_`：真实在线角色集合。
- `players_by_role_`：角色数据 cache。

关键规则：

- 登录成功后角色进入 online 集合。
- 登出时移除 online，并标 dirty。
- `online_count()` 基于 online 集合，不基于 cache 数量。
- 玩家对象数据序列化为 JSON 后保存。
- 保存失败必须 `LOG_ERROR`。
- Redis 是外部服务进程，zone 只连接使用。

## World Ownership Fencing

world 只保存给 web/选区用的轻量状态，不保存玩家完整运行数据。

role ownership 需要抵抗以下乱序：

- 玩家从 zone A 切到 zone B 后，zone A 的迟到 logout 不能清掉 zone B ownership。
- 同一 zone 内旧 session 的迟到 logout 不能清掉新 session ownership。

当前接口：

```cpp
world_set_player_zone(context,
                      role_id,
                      zone_service_id,
                      source_zone_service_id,
                      gateway_session_id);
```

fencing 逻辑：

- logout 时，如果 source zone 不是当前 zone，则忽略。
- logout 时，如果 gateway session 不是当前 session，则忽略。
- login/update 时记录最新 zone/session。

`world/model/world_ownership_store.*` 提供可插拔 store：

- `InMemoryWorldOwnershipStore`：单进程/测试使用。
- `RedisWorldOwnershipStore`：使用 Redis Lua CAS 语义，避免多 world 或 world 重启后 fencing 信息只存在内存里。

`game_world_server` 通过 `world_ownership_store` 配置选择 store：

- `memory`：默认模式，适合本地和单 world 测试。
- `redis`：使用 Redis Lua CAS，适合多 world 或 world 重启后仍要保留 fencing 的场景。

如果直接构造 `WorldMsgContext` 且未设置 `ownership_store`，仍会使用 context 内部 map 行为。

## 容灾与恢复

### Tunnel 注册重试

`global/world/zone` 注册 tunnel 时不因首次失败退出，而是后台循环：

- 成功后周期刷新注册。
- 失败后短间隔重试并记录错误。
- tunnel service 对同 service id 的重复注册做覆盖更新。

### Tunnel endpoint failover

`ProcessMessageManager::call_tunnel(...)` 会：

- 优先尝试 alive tunnel。
- 没有 alive 或需要 probe 时也会尝试 dead tunnel。
- 当前 endpoint 无响应或返回 `RpcStatus::unavailable` 时尝试下一个。
- 记录 attempts/retries/recoveries/failures。
- 在重试之间做小的 bounded backoff。

### Gateway 到 Zone 重试

gateway direct RPC 到 zone 时：

- 同一 endpoint 最多尝试 3 次。
- 无响应或 `unavailable` 会重试。
- 恢复成功会记录 recovered 日志。
- 记录 attempts/retries/recoveries/failures。

### Zone freshness 与 admission

world 的 zone 选择只是前置优化，不能保证绝对容量正确。最终准入在 zone：

- world 根据 zone 定时负载和 pending reservation 选区。
- zone 根据真实 `online_count()` 和 `zone_max_players` 做 admission。
- zone 满载时返回 `RpcStatus::unavailable`。

### Metrics 日志

`ProcessMessageManager` 和 `GatewayServerService` 都维护基础 counters：

- attempts
- retries
- recoveries
- failures
- active gateway connections
- active gateway sessions

`metrics_log_interval_ms` 控制进程是否周期输出 metrics 日志：

- `0`：关闭周期日志，只在 stop 时输出 summary。
- `>0`：按配置间隔输出 world tunnel metrics 或 gateway zone metrics。

当前是日志可见性和结构化 accessor，不是 Prometheus exporter。后续如果接 metrics 系统，可以直接复用已有 counters/accessors。

### Graceful Drain 基础状态

当前已实现“停止接新请求”的 drain 基础状态：

- gateway stop 时进入 draining。
- world stop 时进入 draining。
- gateway draining 后拒绝新 login/game/push。
- gateway stop 会调用 `RpcNetworkServer::close_all_connections()` 主动关闭 active client connections。
- world draining 后 HTTP `/game/login_options` 返回 service unavailable。

这仍不是完整商业级 drain。还需要按业务策略补 pending request 等待上限、超时踢下线、push flush/ack、可靠 logout 补偿。

## 外部 HTTP API

web 是完整独立的外部 HTTP 服务，不注册 GameServer RPC route，不连接 tunnel，也不需要内部 `GameServiceId`。它只根据配置连接 Redis 和 world HTTP。

web 对客户端提供：

```text
GET  /bootstrap?player_uid=<uid>
POST /register
POST /login
```

`POST /register` 和 `POST /login` 请求体：

```json
{ "account": "alice", "password": "pw" }
```

成功返回账号结果和登录选项：

```json
{
  "ok": true,
  "player_uid": 42,
  "message": "login ok",
  "gateways": [],
  "roles": [],
  "zones": []
}
```

web 登录/注册成功后，通过 HTTP 请求 world 获取游戏登录选项。

world 监听 HTTP，给外部 web 使用：

```text
GET /game/login_options?player_uid=<uid>
```

成功返回 JSON：

```json
{
  "ok": true,
  "gateways": [
    { "service_id": 0, "host": "127.0.0.1", "port": 30001, "name": "gateway-a" }
  ],
  "roles": [
    { "role_id": 10001, "name": "knight", "level": 12, "world_service_id": 0, "zone_service_id": 0 }
  ],
  "zones": []
}
```

错误返回：

```json
{ "ok": false, "error": "..." }
```

HTTP 解析、参数、路由、body 读取复用 `HttpProto` API。

## 数据所有权

```text
Redis
  account/auth data
  role summary or ownership fencing when configured
  persisted player JSON from zone

World
  role summary for login options
  current role -> zone/session fence
  zone load/freshness/reservation state

Zone
  authoritative online player runtime data
  dirty player cache
  flush to Redis on timer/stop

Gateway
  client session table
  gateway_session_id -> role/zone/connection route
```

原则：

- 玩家运行数据属于 zone。
- world 不承载完整玩家对象。
- gateway 不承载玩家业务状态，只承载连接和 session route。
- tunnel 不理解业务，只做内部路由基础设施。

## 测试覆盖

主要测试在 `libs/game/server/test/`：

- `test_game_server_login_flow.cpp`
  - gateway session id 分配。
  - invalid session 拒绝。
  - CS frame header mismatch 拒绝。
  - valid CS frame game request 通过。
  - replayed CS frame sequence 拒绝。
  - `gateway.push` 对 active session 成功。
  - reservation 分散 burst login。
  - duplicate login options 复用同 role reservation。
  - reservation TTL prune。
  - stale zone TTL。
  - ownership/session fencing。

- `test_game_server_tunnel.cpp`
  - service id route。
  - random_one。
  - all_of_type broadcast。
  - async reply。
  - tunnel endpoint retry。
  - retry/recovery metrics。
  - down tunnel endpoint 恢复后 reconnect。

- `test_game_server_smoke.cpp`
  - 启动 tunnel/global/world/zone/gateway/mock client 多进程 smoke。
  - 使用 JSON 模板动态改端口。
  - 覆盖 web/world login options、gateway login、GM、game forward、time sync、logout。
  - mock client 使用同一个 gateway TCP 连接连续发送 login/game/time/logout，覆盖 gateway 长连接 RPC frame stream。

- `test_game_server_network_smoke.cpp`
  - Core RPC persistent connection 上连续 request/response。
  - `active_connection_count()` 连接跟踪。
  - `close_all_connections()` drain 主动关闭连接。

- `test_game_server_client_frame.cpp`
  - CS frame encode/decode roundtrip。
  - bad magic 拒绝。
  - sequence pass/replay reject/next sequence pass。
  - oversized payload 拒绝。
  - CS stream decoder 半包、粘包、bad magic/version、oversized payload。

- `test_game_server_world_ownership.cpp`
  - in-memory ownership store zone/session fencing。
  - `WorldMsgContext` 接入 ownership store 后忽略 stale logout。

- `test_game_server_redis_ownership.cpp`
  - Redis ownership store Lua CAS fencing。
  - Redis 不可用时自跳过。

常用验证命令：

```bash
cmake --build build --target game_global_server game_zone_server game_world_server game_gateway_server game_mock_client test_game_server_smoke test_game_server_login_flow test_game_server_tunnel test_game_server_client_frame test_game_server_world_ownership test_game_server_redis_ownership
ctest --test-dir build -R "game_server_redis_ownership|game_server_client_frame|game_server_world_ownership|game_server_login_flow|game_server_tunnel|game_server_smoke" --output-on-failure -V
```

## 当前已知边界

当前实现已经具备本地多进程骨架、gateway 持久 TCP/YuanRpc 收包路径、connection-id session 绑定、push 写回连接、断线 cleanup、连接数/缓冲/idle 控制和 drain 关闭连接基础能力，但还不是完整商业长连接网关。需要继续补齐的方向：

- gateway 连接生命周期细化：认证前超时、慢客户端分级处理、按 IP/账号维度限额。
- gateway push 写入的背压处理、失败重试/丢弃策略和客户端 push ack。
- 断线 cleanup 的重试与补偿：zone/world logout 失败时的可靠队列或延迟重试。
- 客户端协议 fuzz 测试、rate limit、replay window 策略、packet-size 策略细化。
- Prometheus exporter。
- 更完整 graceful shutdown/drain：pending request 等待、超时关闭、flush session、push ack/flush。
- 多 world 部署下的 Redis ownership store 长时间运行测试。
- 更完整的 Redis role/account/player schema。

这些边界不是设计缺陷，而是下一阶段生产化工作的明确接缝。
