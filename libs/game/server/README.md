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
- gateway 负责客户端连接会话、登录态校验、为客户端包补内部 gateway session 上下文并转发到 zone。
- zone 负责玩家在线运行数据、准入控制、业务执行和定时持久化。
- tunnel 负责内部 service id 注册、发现、转发、广播和异步 reply。
- Redis 是外部进程，GameServer 不内嵌也不启动 Redis。

## 目录结构

```text
libs/game/server/
  common/        公共业务协议、service id、配置解析、RPC 网络适配、可选 CS frame codec
    codec/       协议序列化 backend；当前默认 binary，预留 protobuf/json 类型
    proto/       通用协议/DTO 结构；CS* 为客户端协议，SS* 为服务间/内部协议，其他中性命名用于 HTTP/展示 DTO
  messaging/     通用进程间消息能力，封装 tunnel endpoint、heartbeat、failover
  tunnel/        内部服务注册与 service id 转发基础设施
  global/        全局服务示例，GM 转发/全局功能入口
  world/         选区、角色轻量状态、登录选项、ownership fencing、HTTP login_options
  zone/          玩家运行数据、登录准入、业务执行、数据 flush
  gateway/       外部客户端入口，连接/session 表、直连 zone
  web/           外部 web/auth 方向入口，通过 world 获取游戏登录选项；handler/service/app 分层
  client/        本地 smoke/mock client
  config/        JSON 示例配置，构建时复制到 build 输出目录
  test/          GameServer 对应测试
```

服务内部继续按职责分层：

- `app/`：进程启动、配置、生命周期、网络/Redis/tunnel/http wiring。
- `rpc/`：内部 RPC 接口注册、decode、调用 handler、encode response。
- `handler/`：web 侧 HTTP handler/controller 注册与实现。
- `service/`：web 侧业务服务，例如注册、登录、bootstrap 聚合。
- `model/`：业务对象、缓存、会话表、ownership store 等业务状态。

## 服务拓扑

```mermaid
flowchart LR
    Client[Game Client] --> Gateway[Gateway Process]
    Browser[Web/Auth Client] --> Web[Web Process]
    ChatClient[Chat HTTP Client] --> ChatWeb[Chat Web Process]

    Web -->|HTTP bootstrap/login/create_role| World[World Process]

    Gateway -->|direct RPC by internal zone route| ZoneA[Zone Process A]
    Gateway -->|direct RPC by internal zone route| ZoneB[Zone Process B]

    Global[Global Process] --> Tunnel[Tunnel Process]
    World --> Tunnel
    ZoneA --> Tunnel
    ZoneB --> Tunnel
    Rank[Rank Process] --> Tunnel
    PlayerDb[Player DB Proxy] --> Tunnel
    WorldDb[World DB Proxy] --> Tunnel
    GlobalDb[Global DB Proxy] --> Tunnel

    Tunnel --> Global
    Tunnel --> World
    Tunnel --> ZoneA
    Tunnel --> ZoneB
    Tunnel --> Rank
    Tunnel --> PlayerDb
    Tunnel --> WorldDb
    Tunnel --> GlobalDb

    Web --> Redis[(Redis)]
    ChatWeb --> Redis
    Rank --> Redis
    ZoneA -->|player data RPC| PlayerDb
    ZoneB -->|player data RPC| PlayerDb
    World -->|read model RPC| WorldDb
    World -->|ownership CAS RPC| WorldDb
    Global -->|global config/audit RPC| GlobalDb
    PlayerDb --> Redis
    WorldDb --> Redis
    GlobalDb --> Redis
```

当前关键边界：

- `gateway` 不连接 world，不连接 tunnel，只根据配置直连 zone。
- `web` 不依赖 GameServiceId/tunnel 这些内部字段，只知道 world HTTP 地址。
- `world/zone/global` 这类业务进程不直接依赖 Redis schema；玩家数据、world read model、global 数据都通过对应 db proxy。`web/chat_web/rank/db_proxy` 是允许直连 Redis 的边界进程；db proxy 的 Redis IO 必须通过 executor/actor 边界，不能阻塞 RPC event loop。
- 客户端只看到 gateway 地址、角色基础信息、登录 token 和 zone 展示状态；不会收到或回传 `zone_service_id`、`gateway_session_id`。
- `world/global/zone` 会注册到 tunnel，首次失败不退出，后台重试。
- `zone` 注册/上报给 world 时带自身负载和对外 gateway 地址。
- 一个 gateway 可以服务多个 zone，一个 zone 当前配置约束只对应一个 gateway。

## 进程与职责

| 进程 | 主要职责 | 对外/对内关系 |
| --- | --- | --- |
| `game_tunnel_server` | 服务注册、service id 路由、随机/广播转发、异步 reply | 内部 RPC 基础设施 |
| `game_global_server` | 全局服务与 GM 示例入口 | 注册到 tunnel |
| `game_world_server` | 登录选项、选区、zone freshness、reservation、role ownership fencing、HTTP API | 注册到 tunnel，同时监听 HTTP |
| `game_zone_server` | 玩家在线状态、zone admission、业务执行、定时 flush、上报 world | 注册到 tunnel，通过 player_db_proxy 持久化玩家数据 |
| `game_gateway_server` | 客户端入口、连接/session 校验、登录态校验、转发到 zone | 对客户端暴露，对 zone direct RPC |
| `game_web_server` | 外部 web/auth 方向入口 | 连接 Redis 和 world HTTP |
| `game_rank_server` | 游戏排行榜和角色简要信息服务，使用 GameServer RPC/binary 协议 | 注册到 tunnel，连接 Redis，供其他业务服拉取 |
| `game_chat_web_server` | 外部聊天 HTTP API、频道订阅、消息、撤回、Redis pub/sub 事件 | 连接 Redis，不经过游戏服 |
| `game_player_db_proxy_server` | 玩家数据持久化代理 | 注册到 tunnel，隔离 zone 对玩家 Redis/MySQL schema 的直接访问 |
| `game_world_db_proxy_server` | world 域轻量数据代理 | 注册到 tunnel，隔离 role location/online/session 等 world read model 存储 |
| `game_global_db_proxy_server` | global 域持久化代理 | 注册到 tunnel，保存全局配置、审计、封禁等跨 world 数据 |
| `game_mock_client` | 本地 smoke 客户端 | 模拟 web/world/gateway/zone 完整路径 |

## 规划中的 MMO 服务

以下服务是 MMO 完整形态需要补齐的服务边界，当前只作为架构设计和协议预留方向，不加入 `start_all.*` / `stop_all.*`，也不要求本地 smoke 启动。落地顺序建议先做目录/target/空 RPC 骨架，再逐步接入 world/zone/gateway。

| 规划进程 | 主要职责 | 对外/对内关系 | 当前状态 |
| --- | --- | --- | --- |
| `game_battle_manager_server` | 战斗房间/副本/竞技场编排，分配 battle instance，维护 battle 生命周期和容量 | 注册到 tunnel；world/zone/match 调它创建/加入/关闭战斗 | 设计预留，暂不启动 |
| `game_battle_server` | 真正跑战斗 tick、帧同步、状态推进、战斗广播、结算初稿 | 注册到 tunnel；gateway 仍不直连，zone 转发客户端战斗输入 | 设计预留，暂不启动 |
| `game_battle_validator_server` | 离线/旁路战斗校验、反作弊复算、关键帧抽检、结算审计 | 注册到 tunnel；battle 提交 replay/摘要，validator 返回校验结果 | 设计预留，暂不启动 |
| `game_mq_server` | 内部可靠消息队列、延迟消息、补偿任务、跨服事件分发 | 注册到 tunnel；业务服通过领域 RPC 投递/确认/消费 | 设计预留，暂不启动 |
| `game_map_server` | 地图分片、AOI、实体可见性、场景对象、传送点和地图实例元数据 | 注册到 tunnel；zone/world 查询地图与场景路由 | 设计预留，暂不启动 |
| `game_match_server` | 匹配、组队排队、跨区战斗入口选择 | 注册到 tunnel；world/global/zone 可调用 | 设计预留，暂不启动 |
| `game_scene_server` | 大世界场景逻辑、NPC/怪物/采集物、非战斗场景 tick | 注册到 tunnel；可按 map shard 分实例 | 设计预留，暂不启动 |
| `game_social_server` | 好友、黑名单、关系链、最近联系人、推荐关系 | 注册到 tunnel 或独立 HTTP；强持久化走 social_db_proxy | 设计预留，暂不启动 |
| `game_guild_server` | 公会、成员、职位、申请、公告、公会活动入口 | 注册到 tunnel；强持久化走 guild_db_proxy 或 global_db_proxy 子域 | 设计预留，暂不启动 |
| `game_mail_server` | 系统邮件、玩家邮件、附件领取、过期清理 | 注册到 tunnel；当前个人邮件落 `player_db_proxy`，全局/运营邮件落 `global_db_proxy`；未来可按规模拆 mail_db_proxy | 设计预留，暂不启动 |
| `game_trade_server` | 交易、拍卖行、摆摊、订单撮合、冻结/解冻资产 | 注册到 tunnel；资产最终落库必须经 db proxy/事务边界 | 设计预留，暂不启动 |
| `game_activity_server` | 活动配置、限时活动状态、跨服活动入口 | 注册到 tunnel；配置来自 global_db_proxy | 设计预留，暂不启动 |
| `game_analytics_server` | 行为日志、战斗日志、经济日志聚合和异步落盘 | 消费 `game_mq_server` 事件，不阻塞业务主链路 | 设计预留，暂不启动 |

规划 service type 建议继续使用 packed service id，不复用已有 type：

| Service type | 建议枚举值 | 说明 |
| --- | --- | --- |
| `battle_manager` | 15 | 战斗编排，不跑 tick |
| `battle` | 16 | 战斗帧同步实例服，不做全局调度 |
| `battle_validator` | 17 | 战斗复算/校验/反作弊 |
| `mq` | 18 | 内部可靠消息队列 |
| `map` | 19 | 地图分片和 AOI 元数据 |
| `scene` | 20 | 大世界场景逻辑 |
| `social` | 21 | 关系链 |
| `guild` | 22 | 公会 |
| `mail` | 23 | 邮件 |
| `trade` | 24 | 交易/拍卖 |
| `activity` | 25 | 活动 |
| `analytics` | 26 | 日志/分析 |

这些枚举值当前先写在文档里，不直接改 `GameServiceType`，避免未实现服务进入编译和配置约束。真正落地某个服务时，再同步修改 `common/service_id.h`、`common/service_config.h` 和 `common/game_rpc_protocol.h`。

## 战斗体系设计

战斗体系分三层：`battle_manager` 只做调度，`battle` 只跑一个或多个战斗实例 tick，`battle_validator` 做旁路复算和审计。这样可以避免 world/zone 混入高频 tick，也避免 validator 阻塞战斗主链路。

推荐职责边界：

- `battle_manager` 负责创建战斗、选择 battle instance、分配 battle_id、记录 battle_id -> battle_service_id、处理战斗关闭和容量上报。
- `battle_manager` 不处理玩家输入，不保存每帧状态，不做结算复算。
- `battle` 负责固定 tick、输入排序、帧号推进、状态快照、delta 广播、replay 摘要生成、结算草案。
- `battle` 不直接写玩家强持久化数据；结算奖励写入仍通过 zone/player_db_proxy 或专门 settlement proxy。
- `battle_validator` 接收 replay 或关键帧摘要，异步复算战斗结果，输出 pass/fail、可疑原因和审计记录。
- `battle_validator` 不在正常战斗 tick 中同步调用；失败结果通过 mq/GM/风控流程处理。
- `zone` 仍是玩家在线归属和 gateway session 的主入口；玩家进入战斗时，zone 把输入转发到 battle，battle 的广播通过 zone/gateway 回客户端。
- `world` 只关心角色在哪个 zone、是否在战斗、战斗入口展示和必要的跨服路由，不参与每帧推进。

推荐战斗消息流：

```mermaid
sequenceDiagram
    participant C as Client
    participant G as Gateway
    participant Z as Zone
    participant T as Tunnel
    participant BM as BattleManager
    participant B as Battle
    participant V as BattleValidator
    participant MQ as MQ

    C->>G: battle enter/request
    G->>Z: gateway.game.forward
    Note over G,B: gateway 不直连 battle；battle/match 等规划服务只注册到 tunnel
    Z->>T: tunnel.forward battle_manager.create_or_join
    T->>BM: battle_manager.create_or_join
    BM->>T: tunnel.forward battle.instance.reserve
    T->>B: battle.instance.reserve
    B-->>T: battle_id + battle_service_id
    T-->>BM: battle_id + battle_service_id
    BM-->>T: battle assignment
    T-->>Z: battle assignment
    Z-->>G: enter battle ok
    C->>G: input(frame_input)
    G->>Z: gateway.game.forward
    Z->>T: tunnel.forward battle.input.append
    T->>B: battle.input.append
    B->>B: fixed tick + frame sync
    B->>T: tunnel.forward battle.frame.broadcast
    T->>Z: battle.frame.broadcast
    Z->>G: gateway.push
    B->>MQ: battle.replay.committed
    MQ->>V: validator.consume replay
    V->>MQ: validation result/audit
```

### 帧同步模型

帧同步采用 server-authoritative lockstep：客户端只上传输入，服务端排序、定帧、广播权威帧。客户端可以本地预测，但最终以服务端帧为准。

核心规则：

- battle 使用固定 tick，例如 `33ms` 或 `50ms`，不跟随客户端 FPS。
- 每个输入带 `battle_id`、`role_id`、`client_frame`、`input_seq`、`input_flags`、`input_payload`、`client_timestamp_ms`。
- battle 以 `server_frame` 为权威帧号，按到达时间窗口和 role/input_seq 去重排序。
- 输入迟到时进入下一可接受帧；超过 rollback window 的输入拒绝或标记 late。
- 每 N 帧生成 state hash；关键帧生成 snapshot 或 replay marker。
- battle 广播 `server_frame`、本帧输入列表、state hash、可选 delta/snapshot id。
- 客户端断线重连时，zone 查询 battle 当前 `battle_id` 和最新 snapshot，再恢复到最近权威帧。

初始协议建议：

| Route | 方向 | 说明 |
| --- | --- | --- |
| `battle_manager.create` | zone/match -> battle_manager | 创建战斗或副本实例 |
| `battle_manager.join` | zone -> battle_manager | 玩家加入已有战斗 |
| `battle_manager.leave` | zone -> battle_manager | 玩家离开/掉线 |
| `battle.instance.reserve` | battle_manager -> battle | 预留实例容量 |
| `battle.input.append` | zone -> battle | 追加客户端输入 |
| `battle.frame.get` | zone -> battle | 重连拉取帧/snapshot |
| `battle.result.commit` | battle -> zone/world/mq | 提交结算草案和 replay 摘要 |
| `battle_validator.submit` | battle/mq -> validator | 提交 replay 或关键帧摘要 |
| `battle_validator.result.get` | GM/global -> validator | 查询校验结果 |

战斗数据边界：

- 高频帧数据优先保存在 battle 进程内存和本地滚动 replay 文件，不能每帧写 Redis。
- 战斗结算走领域化 RPC，不允许 battle 直接写玩家 Redis schema。
- replay 长期保存可进入对象存储或 analytics pipeline，`battle_validator` 只消费摘要/文件引用。
- 跨服竞技场可以由 `match` 选择 `battle_manager`，再由 `battle_manager` 选择具体 `battle`。

## 消息队列服设计

`game_mq_server` 是内部可靠异步边界，解决“不能同步阻塞主链路，但必须最终处理”的问题。它不是替代 tunnel；tunnel 负责同步 RPC/转发，mq 负责可靠事件、补偿和延迟任务。

核心职责：

- 可靠投递：业务服提交事件后拿到 `message_id`。
- 消费确认：消费者 ack 后才删除或推进 offset。
- 延迟消息：支持 `not_before_ms`，用于延迟 logout 补偿、邮件过期、活动结算。
- 幂等键：支持 `dedupe_key`，避免重复发奖、重复邮件、重复审计。
- 死信队列：超过 retry 上限进入 DLQ，供 GM/运维处理。
- 分区顺序：按 `topic + partition_key` 保证同一玩家/公会/战斗的局部顺序。

初始 topic 建议：

| Topic | 生产者 | 消费者 | 用途 |
| --- | --- | --- | --- |
| `player.logout.compensate` | gateway/zone | zone/world | 断线清理失败后的补偿 |
| `battle.replay.committed` | battle | battle_validator/analytics | 战斗 replay 校验和日志 |
| `battle.result.audit` | battle_validator | global/analytics | 战斗校验结果 |
| `mail.send` | global/activity/gm | mail | 系统邮件异步发送 |
| `economy.audit` | trade/zone | analytics/global | 经济流水审计 |
| `guild.event` | guild | social/analytics | 公会事件广播 |

存储边界：

- 本地开发可先使用 Redis Stream/List 做 mq 后端，但 mq schema 只属于 `game_mq_server`。
- 业务服不直接写 mq 的 Redis key。
- 正式环境可替换为 Kafka/Pulsar/RabbitMQ 后端，业务 RPC 不变。

## 地图与场景服务设计

`game_map_server` 保存地图静态/半静态元数据和分片路由，`game_scene_server` 跑大世界场景 tick。二者不要混成一个巨大 zone。

`map` 负责：

- map_id、line_id、shard_id 到 scene/zone 的路由。
- 地图配置版本、出生点、传送点、复活点、安全区、阻挡数据版本。
- AOI 分区参数，例如 cell size、可见半径、跨 cell 订阅策略。
- 地图实例创建策略，例如公共地图、队伍副本、公会领地、活动地图。

`scene` 负责：

- 大世界 NPC、怪物、采集物、机关、动态事件。
- AOI 订阅和实体可见性广播。
- 非战斗移动、普通技能预演、触发器、采集、交互。
- 地图线负载上报，支持 world/map 做分线选择。

与现有 zone 的关系：

- zone 继续保存玩家在线数据和 gateway session，是客户端业务包入口。
- scene 不直接面对 gateway；客户端场景包仍经 gateway -> zone，再由 zone 转发/调用 scene。
- 如果后续要极致性能，可把特定场景地图直接合并进 zone instance，但要保持协议边界不变。
- map/scene 不直接写玩家强持久化数据，背包、经验、任务进度仍经 zone/player_db_proxy 或对应领域 proxy。

## MMO 补充服务落地顺序

建议按依赖和风险分阶段，不一次性全部启动：

1. `game_mq_server` 骨架：先支持可靠事件、ack、retry、DLQ，为后续补偿和异步审计打基础。
2. `game_map_server` 骨架：先支持 map route 查询和地图配置版本，不跑 tick。
3. `game_battle_manager_server` 骨架：支持创建/查询 battle assignment。
4. `game_battle_server` 最小帧同步：固定 tick、输入 append、帧广播、replay 摘要。
5. `game_battle_validator_server`：消费 replay 摘要并异步输出 pass/fail。
6. `game_scene_server`：从 map route 接入 AOI 和场景实体。
7. social/guild/mail/trade/activity/analytics：按产品优先级逐个拆服务和 db proxy。

上线原则：

- 新服务默认只注册到 tunnel，不加入 gateway 直连。
- 客户端入口仍只有 gateway，除 web/chat_web 这类外部 HTTP 服务外不暴露内部端口。
- 强持久化数据必须经领域 db proxy，不允许新业务服直接依赖 Redis/MySQL schema。
- 高频 tick 服只做内存推进和批量摘要，不能把 Redis/DB IO 放进 tick loop。
- 新服务必须先有本地单元测试和不启动 smoke，再决定是否加入 `start_all.*`。

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
- world/gateway/zone 内部路由和 ownership fencing。
- gateway 根据连接上下文维护 `connection_id -> gateway_session_id -> zone_service_id`。
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
- 登录 token 签名密钥：`login_token_secret`，world/gateway 必须配置一致；token 包含过期时间并由 gateway 验签，未配置时使用内置默认值。可用环境变量 `GAME_LOGIN_TOKEN_SECRET` 覆盖。
- gateway 内部路由密钥：`gateway_internal_secret`，用于 `gateway.push` / `gateway.session.close` 等内部 route metadata 校验；可用环境变量 `GAME_GATEWAY_INTERNAL_SECRET` 覆盖。
- world 路由：`world_routing_strategy`、`world_routing_version`、`world_count`。当前生产方案使用 `modulo`，即 `world = 1 + player_uid % world_count`，world 编号固定为 `1..world_count`。
- world zone freshness：`zone_report_ttl_ms`。
- world ownership store：正式配置使用 `world_ownership_store=proxy`，通过 `world_db_proxy` 执行 ownership CAS。`memory` 仅用于单进程/单测场景。业务 world 进程不直接连接 Redis。
- zone 负载同步：`zone_load_sync_interval_ms`。
- zone 准入容量：`zone_max_players`。
- tunnel heartbeat：`tunnel_heartbeat_interval_ms`。
- 周期 metrics 日志：`metrics_log_interval_ms`，`0` 表示关闭。

配置约束：

- 正式配置不接受无效 host 或端口 `0`。
- `global/world/zone` 使用 `tunnel_endpoints` 数组，可配置多个 tunnel。
- `gateway` 使用 `zone_endpoints`，不配置 tunnel/world。
- `zone` 必须配置且只能配置一个 `gateway_endpoints`，用于上报给 world。
- `web` 使用显式 `world_endpoints` 加 world routing 配置选择固定 world HTTP endpoint；`world_count` 是配置分片数，不随某个 world 存活状态变化。
- JSON `world_endpoints` 形态为 `{ "world": 1, "host": "127.0.0.1", "port": 25103, "state": "open" }`；旧 key/value 配置形态为 `world_endpoints=1,127.0.0.1,25103,open;2,127.0.0.1,25104,open`。
- 扩容/缩容 modulo world 必须停服调整 `world_count` 和 `world_routing_version`，不能运行中因为某个 world down 改变取模基数。
- zone 上报负载时携带 routing 配置；world 只接受 `world_routing_strategy`、`world_routing_version`、`world_count` 完全一致的 zone 注册。

## 内部 RPC 与 tunnel 路由

内部 RPC route 使用数字协议，定义在 `common/game_rpc_protocol.h`。字符串 route name 只作为 debug/fallback 信息。

RPC wire 层只承载 `yuan::rpc::Bytes payload`，不绑定具体序列化格式。GameServer 当前默认使用 `common/codec/game_binary_codec.*`，底层 reader/writer 在 `common/codec/binary_codec.h`。未来接 protobuf/json 时应新增 codec backend，而不是修改 `libs/rpc` wire 协议。

协议组织边界：

- `common/proto/*` 放协议结构、通用 DTO 和基础类型；普通协议结构自己实现 `binary_encode(...)` / `binary_decode(...)`。
- `common/codec/*` 放序列化 backend 和通用入口，当前有 `encode_binary(...)` / `decode_binary<T>(...)` 和 `GameCodecType`。
- 简单二进制协议用 `YUAN_GAME_BINARY_FIELDS(...)` 声明 wire 字段，一次生成 `binary_encode(...)` / `binary_decode(...)`，避免每个协议手写重复 codec。
- 游戏客户端边界协议使用 `CS` 前缀，例如 `CSLoginRequest`。
- 服务间/内部协议使用 `SS` 前缀，例如 `SSPlayerZoneUpdate`、`SSGmCommandRequest`。
- gateway 不解客户端 `CS*` payload，也不要求客户端额外传 `ClientFrameHeader`；gateway 只按 `rpc.connection_id` 维护连接/session，并把客户端 payload 透传给 zone。
- gateway 到 zone 的内部路由上下文通过 RPC metadata 传递，例如 `gateway.session_id`；gateway 不处理 `player_uid/role_id` 业务语义，这些由 zone 解客户端 payload 后校验和保存。
- `GatewayMsgContext` 只保存 gateway 状态和配置，不保存 login/game/logout/time 业务 handler；注册 gateway route 时显式注入 zone 选择、zone 转发、client push 三个基础能力。
- `gateway.time_sync` 不在 gateway 修改时间或解请求，只按已登录 session 透传到 `zone.time_sync`，由 zone 生成响应。
- HTTP/展示 DTO 不强行使用 `SS`，例如 `LoginOptionsResponse`。
- route handler 不应假设 RPC wire 是 binary/protobuf/json，只处理解码后的协议对象或显式调用对应 codec。
- RPC route 注册点应优先绑定命名 handler 函数，避免把完整业务逻辑写成内联 lambda。lambda 只适合很小的适配闭包，例如异步 Redis 完成后捕获 request id/connection id 写 deferred response。
- 新增普通二进制协议时不需要改 `game_binary_codec.h/.cpp`：在协议结构内用 `YUAN_GAME_BINARY_FIELDS(field1, field2, ...)` 声明 wire 字段，上层统一调用 `encode_binary(obj, bytes)` / `decode_binary<Type>(bytes)`。
- 只有 wire layout 不同或需要兼容多种格式的协议才保留专用 codec 函数。

当前 `YUAN_GAME_BINARY_FIELDS(...)` 是轻量的“本地生成器”：字段顺序就是 wire 顺序，支持基础类型、`std::string`、`yuan::rpc::Bytes`、`std::vector<T>` 和嵌套协议结构。后续如果协议继续增长，可以把同一份字段清单迁移到 `.proto/.yaml/.json` schema，再生成 C++ struct 和相同的 `binary_encode/binary_decode` 方法；上层调用方式不用变。

tunnel 支持三种转发模式：

- `specific`：指定一个具体 service instance。
- `random_one`：在某个 service type 下随机选一个实例。
- `all_of_type`：广播给某个 service type 下所有实例。

内部消息流：

```mermaid
sequenceDiagram
    participant Z as Zone
    participant DB as DbProxy
    participant T as Tunnel
    participant W as World
    participant G as Global
    participant R as Rank
    participant GW as Gateway

    Z->>T: tunnel.register(zone service_id, endpoint)
    W->>T: tunnel.register(world service_id, endpoint)
    G->>T: tunnel.register(global service_id, endpoint)
    DB->>T: tunnel.register(player/world/global db proxy service_id, endpoint)
    R->>T: tunnel.register(rank service_id, endpoint)
    Note over GW,T: gateway 不连接 tunnel，只按配置 direct RPC 到 zone
    Z->>Z: route target world by player_uid % world_count
    Z->>T: tunnel.forward(target_service_id=computed world, world.player_zone_set)
    T->>W: world.player_zone_set
    W-->>T: RPC response
    T-->>Z: RPC response
    W->>DB: direct RPC when endpoint configured, otherwise tunnel.forward
    DB-->>W: world_db / player_db / global_db response
```

`TunnelClientManager` 在 `messaging/` 中提供服务侧通用能力。`TunnelClient` 只表示一个到单个 `tunnel_server` endpoint 的连接；manager 负责持有多个 client、在发送前选择 client、failover、注册刷新和发送。默认 tunnel 选择策略是 round-robin，也可按调用显式指定 random 或 hash_by_service_id：

- 多 tunnel endpoint 管理。
- heartbeat。
- register/forward/send/random/broadcast。
- 发送前选择 client、failover 与 reconnect probe。
- retry attempts/retries/recoveries/failures 计数。

## 登录与选区流程

当前登录分两段：先从 web/world 获取登录选项，再连 gateway 登录。web 用公共 world routing 按 `player_uid` 固定选择 world；world 不连接 gateway；world 选出 zone 后生成带过期时间和签名的 `login_token_id` 给客户端，客户端首包只带 token，不直接暴露 `zone_service_id`。gateway 验签、检查过期时间并还原 token 得到目标 zone，然后在登录成功后把内部 route context 绑定到真实连接。

`login_token_id` 格式为 `zone_service_id.expires_at_ms.mac`，`expires_at_ms` 是 token 本身的一部分并参与 HMAC-SHA256；gateway 拒绝过期、缺失 expiry 或 MAC 不匹配的 token。

```mermaid
sequenceDiagram
    participant C as Client
    participant Wb as Web
    participant Wo as World
    participant PlayerDb as PlayerDbProxy
    participant WorldDb as WorldDbProxy
    participant Gw as Gateway
    participant Zn as Zone

    C->>Wb: auth/login or bootstrap
    Wb->>Wb: route world by player_uid % world_count
    Wb->>Wo: HTTP GET /game/login_options?player_uid=...
    Wo->>WorldDb: world_db.player_roles.get
    alt no role exists
        WorldDb-->>Wo: empty roles
        Wo-->>Wb: empty roles + public zone status
        Wb-->>C: login options with no roles
        C->>Wb: POST /create_role
        Wb->>Wo: HTTP /game/create_role?player_uid=...&name=...
        Wo->>PlayerDb: player_db.create_role
        Wo->>WorldDb: world_db.player_roles.save read model
        Wo-->>Wb: created role_id
        Wb-->>C: create role ok
        C->>Wb: auth/login or bootstrap again
        Wb->>Wo: HTTP GET /game/login_options?player_uid=...
        Wo->>WorldDb: world_db.player_roles.get
    end
    WorldDb-->>Wo: player role read model
    Wo->>Wo: prune expired reservations
    Wo->>Wo: mark stale zones unavailable
    Wo->>Wo: select least-loaded fresh zone
    Wo->>Wo: create/refresh pending reservation
    Wo-->>Wb: roles(login_token_id) + gateway address + public zone status
    Wb-->>C: login options

    C->>Gw: gateway.login ClientLoginRequest(login_token_id)
    Gw->>Gw: verify/decode login_token_id -> zone + reserve internal gateway_session_id
    Gw->>Zn: zone.player_enter direct RPC (payload透传, metadata带内部session)
    Zn->>Zn: decode ClientLoginRequest + admission control + PlayerManager online
    Zn-->>Gw: ClientLoginResponse + metadata(zone/session)
    Gw->>Gw: bind connection_id -> gateway_session_id -> zone
    Zn->>Zn: route target world by player_uid % world_count
    Zn->>Wo: world.player_zone_set via tunnel
    Gw-->>C: ClientLoginResponse without internal route/session ids
```

选区规则：

- world 不使用 `%` 求余选择 gateway/zone。
- web 和 zone 使用同一份 `common/world_routing.h` 计算玩家归属 world；tunnel 不理解 uid，只按明确的 `target_service_id` 转发。
- world 挂掉时，归属到该 world 的玩家不可用；不能把 down world 从 `world_count` 中删除后重新取模。
- 已登录角色优先保持当前 zone。
- 新登录角色从 fresh、available、未满载的 zone 中选 `online_players + pending_reservations` 最小者。
- 同一个 role 重复请求 login options 会复用并刷新同一 reservation。
- 查询登录选项不隐式创建角色；角色不存在时返回空 roles，由显式创角流程写入 player_db 强持久化和 world_db 登录展示 read model，成功后再重新获取 login options。
- 同一个 player uid 同时只能在线一个 role；world 显式维护 `uid -> online session` 和 `role -> online session`，新 role 上线时会清理同 uid 旧 role 的 ownership/session。
- reservation 到期后会被清理。
- zone 定时上报负载；超过 `zone_report_ttl_ms` 未上报会被标记 unavailable。
- zone 自身还有 admission control，作为最终准入保护。

## Gateway 长连接与会话

gateway 当前对客户端暴露三种 transport：TCP、WebSocket、UDP+KCP。三者都只承载同一套 YuanRpc frame，业务 route、payload、login/session 语义一致；客户端可以按平台或网络状况选择入口，切换 transport 不改变业务协议。

入口选择建议：

- TCP：native/PC 默认长连接入口，延迟低，实现最简单。
- WebSocket：微信小游戏和 H5 的正式入口，使用 binary WebSocket frame 承载完整 YuanRpc frame。
- UDP+KCP：native/PC 弱网优化入口，KCP payload 承载完整 YuanRpc frame；微信小游戏/H5 不走 KCP。

当前 gateway 收包链路：

```text
TCP bytes
  -> RpcNetworkServer per-connection FrameStreamDecoder
  -> complete YuanRpc request frame
  -> rpc::Server route dispatch
  -> gateway handler
  -> lookup connection context by rpc.connection_id
  -> direct RPC to zone with original payload + internal metadata
```

WebSocket 收包链路：

```text
WebSocket binary frame
  -> RpcFrameConnectionDispatcher per-connection FrameStreamDecoder
  -> complete YuanRpc request frame
  -> rpc::Server route dispatch
  -> gateway handler
```

KCP 收包链路：

```text
UDP datagram
  -> Core KcpServerSession handshake/session by conv + address
  -> ikcp_input / ikcp_recv
  -> RpcFrameConnectionDispatcher per-connection FrameStreamDecoder
  -> complete YuanRpc request frame
  -> rpc::Server route dispatch
  -> gateway handler
```

`RpcNetworkServer` 的关键语义：

- 支持半包：`DecodeError::need_more` 时保留 buffer，等待后续 read。
- 支持粘包：一次 read 中有多个完整 YuanRpc frame 时循环处理。
- 支持同一 TCP/WebSocket/KCP session 多次 request/response，不在每次 response 后主动 close。
- 协议错误会关闭当前连接并清理该 connection 的 decoder state。
- 每个 request 会带内部 metadata `rpc.connection_id` 进入 handler，用于绑定 gateway session 到真实 transport connection/session。
- handler 可以返回内部 metadata `rpc.close_connection=1`，adapter 会先写 response 再关闭连接。
- handler 可以返回内部 metadata `rpc.defer_response=1`，adapter 不立即写 response；业务随后调用 `write_response_to_connection()` 写回。gateway 用这个机制把 zone 网络转发放到自己的串行 handler worker，避免网络 runtime 线程等待 zone timeout。
- gateway handler worker 维持单线程 session 状态模型，不默认按连接分片加线程。客户端请求转发和断线 cleanup/retry 共用这个 worker，不再为 cleanup 单独启动线程。为了避免 zone 慢时无限积压，gateway handler queue 是有界队列，配置项 `gateway_handler_queue_limit` 默认 `4096`；队列满时直接返回 `RpcStatus::unavailable`。
- gateway handler worker 内部按 zone endpoint 复用多个 `RpcNetworkPersistentClient` 持久连接；因此是多 zone 连接、无额外 zone client worker。gateway 当前只有 main network runtime + `gateway_handler_thread_` 两条核心线程。
- gateway metrics 暴露 `handler_queue_size`，用于判断是否需要后续按 `connection_id` 分片或改为真正 async zone callback。
- gateway/world 的周期 metrics 日志使用 Core timer，不再为 metrics 单独启动线程。
- `RpcNetworkServerConfig` 支持连接生命周期控制：`max_connections`、`max_buffered_bytes`、`idle_timeout_ms`。
- `idle_timeout_ms` 检查使用 Core timer，不再为 idle monitor 单独启动线程。
- `close_all_connections()` 用于 drain 时主动关闭当前 active connections/sessions。
- `active_connection_count()` 提供结构化 metrics accessor。
- `stop_after_requests` 只用于测试里让 server run loop 退出，不代表生产连接生命周期。
- handler 默认在网络 runtime 线程串行执行。不要在 `RpcNetworkServer` 内部为每个请求创建线程，也不要隐式引入 worker pool；GameServer 上层状态默认按单线程模型编写。
- 如果某个业务确实需要阻塞或耗时任务，应由该业务显式投递到自己的后台队列/线程池，或使用 `rpc.defer_response` 后续写回，并为相关状态加同步保护。
- world 的 HTTP `/game/login_options` 和常规 RPC 绑定到同一个 `RpcNetworkServer` runtime，不再为 world HTTP 单独启动线程。world 状态更新仍回到同一 event loop 执行，避免给 `WorldMsgContext` 引入默认多线程访问模型。
- world HTTP `/game/login_options` 需要角色 read model 时通过 `world_db_proxy` 领域 RPC 获取；world 进程不直接连接 Redis，也不理解 Redis schema。

当前 smoke/mock client 已验证同一个 gateway TCP 连接上连续完成 login、game、time sync、logout；focused transport tests 已验证 WebSocket binary frame 和 UDP+KCP 都能承载 YuanRpc request/response roundtrip。gateway 客户端入口不会在网络 runtime 线程等待 zone 网络 timeout；内部 world/zone/tunnel 的后台或非客户端入口路径仍可继续使用短连接 `RpcNetworkClient::call(...)`。

如果后续要接“非 YuanRpc 包裹”的 raw CS socket，已有 `ClientFrameStreamDecoder` 可以直接复用在 socket read callback 上，实现 CS frame 自身的半包/粘包处理。

客户端到 gateway 的正式 CS 包使用 YuanRpc route + 业务 payload。当前 gateway 客户端入口不要求额外的 `ClientFrameHeader`。客户端 payload 是业务协议 bytes，gateway 不解；gateway 只使用 transport adapter 注入的 `rpc.connection_id` 找连接上下文。

gateway 配置中 `listen_port` 是 TCP 入口，`websocket_port` 是 WebSocket 入口，`kcp_port` 是 UDP+KCP 入口；`websocket_port` 或 `kcp_port` 为 `0` 时对应入口不启动。KCP 多客户端会话管理已经下沉到 Core 的 `KcpServerSession`，gateway 的 `GatewayKcpTransport` 只是薄 YuanRpc adapter。Core 默认 handshake 支持 UDP packet type `1` + magic `YKCP1`，服务端返回 packet type `2` + 4-byte big-endian conv；后续 KCP segment 外层 UDP packet type 为 `3`，KCP 自身 conv 用于 session lookup。gateway 正式模式可配置 `kcp_require_login_token=true`，此时新建 session 的 handshake payload 直接使用 web/world 返回给客户端的 `login_token_id`，gateway 用 `login_token_secret` 解码并校验过期/MAC，客户端不需要额外配置 KCP secret 或单独获取 KCP token。

KCP NAT migration 由 Core 在握手层完成，gateway 只负责校验 payload。开启 `kcp_allow_migration=true` 后，客户端地址变化时可向新 UDP 地址发送 handshake payload `login_token_id|conv`；`login_token_id` 仍按 TTL/MAC 校验，`conv` 是首次 handshake ack 返回的 4-byte conv 值。校验通过后 Core 会把该 KCP session 的地址从旧 `ip:port` 更新为新 `ip:port`，并返回同一个 conv。没有合法 token 的地址不能劫持现有 conv；迁移到已绑定其他 session 的地址会被拒绝。

KCP 相关 gateway 配置包括：`kcp_update_interval_ms`、`kcp_cleanup_interval_ms`、`kcp_idle_timeout_ms`、`kcp_mtu`、`kcp_send_window`、`kcp_receive_window`、`kcp_resend`、`kcp_nodelay`、`kcp_no_congestion_control`、`kcp_max_sessions`、`kcp_max_sessions_per_ip`、`kcp_allow_migration`、`kcp_max_handshakes_per_address_per_window`、`kcp_handshake_rate_window_ms`、`kcp_max_malformed_packets_per_address`、`kcp_require_login_token`。这些配置会直接传给 Core `KcpServerSession`，不是 gateway 私有实现。

压测调参建议：先固定业务 payload 和 RTT/loss profile，再按目标平台调整 `kcp_mtu`、`kcp_send_window`、`kcp_receive_window`、`kcp_update_interval_ms`、`kcp_resend` 和 `kcp_no_congestion_control`。高并发压测要同时设置 `kcp_max_sessions`、`kcp_max_sessions_per_ip`、`kcp_max_handshakes_per_address_per_window`、`kcp_handshake_rate_window_ms` 和 `kcp_max_malformed_packets_per_address`，避免单机压测或攻击流量把 session table 和 handshake path 打满。

Core UDP 底座的生产化边界：

- `UdpConnection` 只负责 UDP datagram connection 抽象，不承载 KCP handshake/conv/session table 语义。
- `UdpConnection` 明确 `remote_address` / `peer_address` 为客户端地址，`local_address` 不再错误复用 peer address。
- `UdpConnectionOptions` 支持 `max_datagram_size`、pending output bytes/datagrams、idle check interval、idle timeout checks。
- `UdpConnectionMetrics` 统计 read/write datagrams、bytes、drop、send/receive errors、created/closed/active connections。
- `DatagramServerSession` 暴露 UDP options、metrics、read/error/close callback、`send_datagram(address, buffer)`。
- `KcpServerSession` 复用 `DatagramServerSession`，并在 Core 层管理 KCP 多客户端会话；上层服务只消费 `connection_id + bytes` callback。

登录时 gateway 只从 `CSLoginRequest` 读取 `login_token_id` 并验签还原目标 zone，然后为连接 reserve 一个内部 `gateway_session_id`，把原始 login payload 和 metadata 转发给 zone。zone 解 `CSLoginRequest` 后把 `zone_service_id/gateway_session_id` 通过 response metadata 回给 gateway，gateway 再绑定：

session table 当前保存：

```text
gateway_session_id -> zone_service_id / connection_id
connection_id -> gateway_session_id
```

`connection_id` 由 `RpcNetworkServer` 注入。gateway login 成功时会记录：`gateway_session_id -> connection_id`。连接关闭后，gateway 会在 gateway handler worker 中串行执行 cleanup：

- 找到该 connection 上的所有 session。
- 向对应 zone 发送 logout。
- 清理本地 session table。

## Gateway Push 骨架

当前已经有 `gateway.push` 内部 RPC route，用于承接 zone -> gateway -> client 的异步推送路径。push 目标通过 metadata 指定，payload 原样写给客户端连接，gateway 不解具体 push 协议。

```mermaid
sequenceDiagram
    participant Z as Zone
    participant G as Gateway
    participant C as Client Connection

    Z->>G: gateway.push(metadata: internal_secret, session_id, payload bytes)
    G->>G: validate gateway.internal_secret
    G->>G: validate session still active
    G-->>Z: ok / not_found / unavailable
    Note over G,C: gateway 根据 gateway_session_id 找到 connection_id 并写 YuanRpc push frame
```

当前行为：

- `gateway.push` 从 metadata 读取 `gateway.session_id`，不解 push payload。
- gateway 校验 `gateway_session_id` 是否仍是当前连接 session。
- `GatewayServerService` 默认 push 能力会把 payload 原样写成 YuanRpc `push` frame 给该 session 对应的 TCP/WebSocket/KCP connection/session。
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
- dirty flush 只有在持久化成功后才清除 dirty 标记；保存失败会保留 dirty，避免定时 flush 或停服 flush 假成功。
- zone load 上报使用 Core timer，不单独启动 load sync 线程。
- zone 停服时先停止网络入口和周期任务，再等待 flush 线程退出，最后在 stop 线程同步 drain dirty 数据；db proxy 必须在 zone 之后关闭，保证最终落库路径还可用。
- zone 玩家加载/保存只走 `player_db_proxy` 领域 RPC；zone 不直接连接 Redis，也不理解玩家 Redis/MySQL schema。

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
- 同 uid 切换 role 后，旧 role 的迟到 logout 不能清掉 `online_by_uid` 里的当前新 role session。
- login/update 时记录最新 zone/session。

`world/model/world_ownership_store.*` 提供两种实现：`InMemoryWorldOwnershipStore` 用于单进程/测试场景，`WorldDbProxyOwnershipStore` 用于正式部署。正式部署下 `game_world_server` 使用 `world_ownership_store=proxy`，通过 `world_db.ownership.compare_and_set` 调 `world_db_proxy`，由 proxy 内部处理 Redis/DB CAS 和 schema。

业务 world 进程不直接连接 Redis。多 world 或重启后仍需要保留 fencing 信息时，必须走 `world_db_proxy` ownership RPC，而不是在 world 中恢复 Redis client。

如果直接构造 `WorldMsgContext` 且未设置 `ownership_store`，仍会使用 context 内部 map 行为。

## 容灾与恢复

### Tunnel 注册重试

注册到 tunnel 的服务不应在业务进程里手写注册循环。统一由 `messaging/TunnelClientManager` 读取服务配置后管理：`set_tunnel_endpoints`、`set_heartbeat_interval_ms`、`configure_registered_service`、`start_registered_service`。业务 service 只在 `start()` / `stop()` 调用 messaging 中间层，不直接构造 `TunnelRegistration` 重试循环。

`global/world/zone/rank/*_db_proxy` 注册 tunnel 时不因首次失败退出，而是由 Core timer 驱动周期任务：

- 成功后周期刷新注册。
- 失败后短间隔重试并记录错误。
- tunnel service 对同 service id 的重复注册做覆盖更新。
- 不为 heartbeat/register 默认额外启动线程。

### Tunnel endpoint failover

`TunnelClientManager::call_tunnel(...)` 会：

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

`TunnelClientManager` 和 `GatewayServerService` 都维护基础 counters：

- attempts
- retries
- recoveries
- failures
- active gateway connections
- active gateway sessions

`metrics_log_interval_ms` 控制进程是否周期输出 metrics 日志：

- `0`：关闭周期日志，只在 stop 时输出 summary。
- `>0`：按配置间隔输出 world tunnel metrics 或 gateway zone metrics。

周期 metrics 日志运行在各自服务的 Core timer 上，不会额外创建 `metrics_thread_`。metrics handler 只读取 counters 并写日志，不能加入阻塞 IO 或重业务逻辑。

当前是日志可见性和结构化 accessor，不是 Prometheus exporter。后续如果接 metrics 系统，可以直接复用已有 counters/accessors。

### Graceful Drain 基础状态

当前已实现 gateway/world drain 基础状态：

- gateway stop 时进入 draining。
- world stop 时进入 draining。
- gateway draining 后拒绝新 login/game/push。
- gateway stop 会先等待已入队 handler、按 session 向 zone 发送 logout flush、等待 gateway->zone pending/queued request 清空，再调用 `RpcNetworkServer::close_all_connections()` 主动关闭 active client connections。
- `gateway_drain_timeout_ms` 控制 gateway drain 总等待上限，默认 3000ms。
- world draining 后 HTTP `/game/login_options` 返回 service unavailable。

RPC 语义上，request 必须返回 response；push/notification 不要求业务回包。当前 gateway push 已做到写入 TCP 输出缓冲并 flush，不能证明客户端业务已处理。如果要强语义 push ack，需要定义客户端 ACK route，并在 gateway/zone/world 维护 ack pending、超时和重试策略。

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

web/chat_web 是外部 HTTP 边界服务，允许直连 Redis。HTTP server 直接在服务主线程 `serve()`，保留 HTTP worker pool 默认 1 个 worker 执行阻塞 handler，避免把 Redis blocking IO 放回 HTTP event loop。

world 的 `/game/login_options` route 是 async HTTP handler。handler 在 world RPC runtime 中进入，角色 read model 通过 `world_db.player_roles.get` 从 `world_db_proxy` 获取；proxy 只返回已存在角色列表，不在读取时自动创建角色。不存在角色时返回空 roles。

显式创角流程是：外部入口调用 web `POST /create_role`，web 转发到 world HTTP `/game/create_role`，world 先确认该 player 当前没有角色，再通过 `player_db.create_role` 创建玩家强持久化角色，随后通过 `world_db.player_roles.save` 写登录展示 read model。两个 proxy 写入都成功后才认为创角成功；之后客户端重新拉 `/game/login_options`，拿到带 `login_token_id` 的角色再走 gateway 登录。

rank 是游戏域服务，承载各种榜单和角色简要信息；它独立成进程，注册到 tunnel，数据放 Redis，其他业务服通过 GameServer RPC/binary 协议拉取：

```text
rank.role.update   SSRankRoleUpdateRequest  -> SSRankRoleResponse
rank.role.get      SSRankRoleGetRequest     -> SSRankRoleResponse
rank.score.update  SSRankScoreUpdateRequest -> SSRankScoreResponse
rank.score.remove  SSRankScoreRemoveRequest -> SSRankScoreResponse
rank.score.get     SSRankScoreGetRequest    -> SSRankScoreResponse
rank.top.get       SSRankTopGetRequest      -> SSRankTopResponse
```

榜单 member 建议使用 `role:<role_id>`；`rank.top.get` 和 `rank.score.get` 会在返回中附带已保存的角色简要信息。

player db proxy 是玩家强持久化数据的领域代理。zone 保留玩家运行态 cache，但 load/save/flush 只通过 `player_db_proxy`；如果 proxy 不可用，保存失败并保留 dirty，不回退到业务进程直连 Redis。proxy 支持多实例部署，业务服通过 `player_db_proxies` group 按 owner id 路由；`target_player_db_proxy_instance` 只作为旧式 fallback。当前第一阶段接口：

```text
player_db.load_role  SSPlayerDbLoadRoleRequest -> SSPlayerDbRoleResponse
player_db.save_role  SSPlayerDbSaveRoleRequest -> SSPlayerDbRoleResponse
player_db.create_role SSPlayerDbCreateRoleRequest -> SSPlayerDbRoleResponse
player_db.mail.create SSPlayerDbMailCreateRequest -> SSPlayerDbMailCreateResponse
player_db.mail.list   SSPlayerDbMailListRequest -> SSPlayerDbMailListResponse
player_db.mail.get    SSPlayerDbMailGetRequest -> SSPlayerDbMailGetResponse
player_db.mail.claim_attachment SSPlayerDbMailClaimAttachmentRequest -> SSPlayerDbMailClaimAttachmentResponse
```

个人普通邮件属于玩家域强持久化数据，当前落在 `player_db_proxy`，不放 `global_db_proxy`。邮件附件领取状态按 `player_uid + role_id + mail_id` 记录，重复领取返回已领取状态但不重复返回附件，业务发奖侧仍要按 `mail_id` 或业务 `dedupe_key` 做幂等。

当前 Redis key 仅在 proxy 内部使用：`game:player_db:player_role:<role_id>`、`game:player_db:mail:<mail_id>`、`game:player_db:mail_index:<player_uid>:<role_id>`、`game:player_db:mail_state:<player_uid>:<role_id>:<mail_id>`、`game:player_db:mail_dedupe:<player_uid>:<role_id>:<dedupe_key>`。后续接 MySQL 或分库分表时，应继续保持 zone 只调用领域 RPC，不暴露库表/key 规则。

world db proxy 是 world 域轻量持久化代理。world 通过 `world_db_proxies` group 选择 proxy；role location 持久化不在登录链路同步等待，而是进入 world dirty flush/retry 队列。当前第一阶段接口：

```text
world_db.role_location.get  SSWorldDbRoleLocationGetRequest -> SSWorldDbRoleLocationResponse
world_db.role_location.set  SSWorldDbRoleLocationSetRequest -> SSWorldDbRoleLocationResponse
world_db.player_roles.get   SSWorldDbPlayerRolesGetRequest  -> SSWorldDbPlayerRolesResponse
world_db.player_roles.save  SSWorldDbPlayerRolesSaveRequest -> SSWorldDbPlayerRolesResponse
```

当前 Redis key 仅在 proxy 内部使用：`game:world_db:role_location:<role_id>`、`game:world_db:player_roles:<player_uid>`、`game:world_db:role_info:<role_id>`。该服务只保存 role location / online session / 登录展示用角色 read model，不保存玩家完整运行对象。

global db proxy 是 global 域持久化代理。初始领域用于全局配置项，后续可扩展 GM audit、封禁状态、全局 ID、配置版本等跨 world 数据：

```text
global_db.config.get  SSGlobalDbConfigGetRequest -> SSGlobalDbConfigResponse
global_db.config.set  SSGlobalDbConfigSetRequest -> SSGlobalDbConfigResponse
global_db.mail.create SSGlobalDbMailCreateRequest -> SSGlobalDbMailCreateResponse
global_db.mail.list   SSGlobalDbMailListRequest -> SSGlobalDbMailListResponse
global_db.mail.get    SSGlobalDbMailGetRequest -> SSGlobalDbMailGetResponse
global_db.mail.claim_attachment SSGlobalDbMailClaimAttachmentRequest -> SSGlobalDbMailClaimAttachmentResponse
```

`global_db_proxy` 的邮件只承载全局邮件和运营邮件，不承载普通个人邮件。全局邮件不推送、不预展开到每个玩家 inbox；玩家或 zone 按固定周期调用 `global_db.mail.list/get` 拉取新增全局/运营邮件，再用玩家维度 state 记录已读/已领。运营邮件通常只有 `title/body/operator_id/operator_reason`，`detail.detail_type=0` 表示没有业务 detail。

邮件通用业务详情使用 `SSMailDetail{detail_type, detail_data}` 包装。`detail_type=0` 表示没有业务详情；非 0 时 `detail_data` 是对应业务的二进制 payload，例如活动补偿、战斗补偿、购买退款等。不要只靠裸二进制判断业务语义。

邮件当前流程：

```mermaid
sequenceDiagram
    participant GM as GM/运营/活动
    participant Z as Zone
    participant PDB as PlayerDbProxy
    participant GDB as GlobalDbProxy
    participant R as Redis/DB
    participant C as Client

    GM->>PDB: player_db.mail.create(player_uid, role_id, detail_type/detail_data, attachments)
    PDB->>R: insert personal mail + player inbox index + dedupe
    C->>Z: mail list/get/claim request
    Z->>PDB: player_db.mail.list/get/claim_attachment
    PDB->>R: load personal inbox / update claim state
    PDB-->>Z: personal mails / claimed attachments
    Z-->>C: mail response

    GM->>GDB: global_db.mail.create(scope=global/operator)
    GDB->>R: insert global/operator mail + global index + dedupe
    Z->>GDB: periodic global_db.mail.list/get(player_uid, role_id)
    GDB->>R: scan global/operator index + player mail state
    GDB-->>Z: new global/operator mails
    Z-->>C: pulled global/operator mails
```

当前 Redis key 仅在 proxy 内部使用：`game:global_db:config:<key>`、`game:global_db:mail:<mail_id>`、`game:global_db:mail_index:global`、`game:global_db:mail_index:operator`、`game:global_db:mail_state:<player_uid>:<role_id>:<mail_id>`、`game:global_db:mail_dedupe:<scope>:<player_uid>:<role_id>:<dedupe_key>`。

db proxy group 配置形态统一，例如：

```json
{
  "player_db_proxies": {
    "strategy": "modulo",
    "version": 1,
    "shard_count": 2,
    "endpoints": [
      { "service_id": 4504702360223745, "shard": 0, "state": "open" },
      { "service_id": 4504702360223746, "shard": 1, "state": "open" }
    ]
  }
}
```

`select_db_proxy(owner_id, routing)` 当前支持 `modulo`。`version` 和 `shard_count` 是配置一致性边界；扩缩容应按停服/迁移计划调整，不应让业务代码散写 modulo。

db proxy 的 Redis backend 使用 `RedisClientPool`，配置字段为 `redis_pool_size`。测试仍可注入单连接 client，生产 proxy 默认初始化连接池。

db proxy RPC handler 不在网络 runtime 中直接执行 Redis command。`player_db_proxy`、`world_db_proxy`、`global_db_proxy` 在服务进程中注入 `RedisAsyncExecutor`，handler 校验请求后返回 `rpc.defer_response=1`，Redis worker 完成 ORM/entity 操作后回到原 RPC runtime，并通过 `write_response_to_connection()` 写回 response。这样 proxy 的 RPC event loop 不会被 Redis IO 阻塞；ORM/entity API 仍保留为同步本地 API，执行位置被限制在 Redis executor worker 内。

`RedisAsyncExecutor` 是 executor/actor 边界，不是 Redis 协议级非阻塞客户端。它内部持有一个 worker thread 和一个同步 `RedisClient`，对调用方表现为 async/offload，目的是隔离 Redis blocking IO；如果后续目标是减少 db proxy Redis worker 线程，需要实现真正基于 event loop 的 Redis async client，而不是继续包同步 client。

db proxy 共享一套 ORM-style 本地 API，放在 `common/storage`，提供 `query/exists/insert/update/upsert/delete_/compare_and_update/batch` 和 Redis-backed `RedisOrmStore`。这套 API 给 proxy 内部复用；业务服仍优先调用领域 RPC，例如 `player_db.save_role`、`world_db.role_location.set`、`global_db.config.set`，不要直接把 SQL/Redis 命令透传给业务服。

ORM 语义约定：

- `insert` 表示必须不存在；Redis backend 用 Lua 一次 RTT 完成 `EXISTS + HSET`，避免非原子 check-then-set。
- `upsert` 表示存在则覆盖、不存在则创建；Redis backend 直接 `HSET`，不做前置 query。
- `compare_and_update` 表示按版本字段 CAS 更新；Redis backend 用 Lua 一次 RTT 完成版本读取、比较和更新。
- `batch` 用于批量 `query/insert/update/delete_`；Redis backend 使用 pipeline 减少 RTT。当前 `transactional=true` 对 Redis ORM 明确返回不支持，避免伪事务语义。
- `EntityStore::save` 是 upsert；需要“必须创建”时用 `EntityStore::insert`，需要乐观锁时用 `EntityStore::compare_and_save`。

在 ORM 之上还有 entity 层：`EntityRecord{table,key,fields,version,object_blob}` 和 `EntityStore::load/insert/save/compare_and_save/remove/save_batch`。新增领域数据时先定义 table/key/fields/version 映射，再在 proxy handler 内调用 entity store；避免每个新结构重复写 Redis key 拼接、JSON 编解码和版本字段处理。

entity 支持直接保存二进制对象：`object_blob` 会映射为字段 `object_blob`。Redis backend 当前存在 hash field 中；MySQL backend 后续可直接映射到 BLOB 列。需要查询/路由的标量仍保留在 `fields`，完整对象可直接存 `object_blob`，例如 `player_db_proxy` 保存 `SSPlayerRoleData` binary payload，同时保留 `player_uid/role_id/level/exp` 字段。

chat web 是独立外部 HTTP 服务，不经过游戏服；频道订阅状态、消息存储和频道事件都在 Redis。当前 HTTP API 提供 durable subscription 和消息历史，实时推送侧可直接订阅 Redis channel `game:chat:pubsub:<channel>`，后续也可以在该进程接 WebSocket/SSE：

```text
POST /chat/subscribe    {"channel":"world","user_id":"90001"}
POST /chat/unsubscribe  {"channel":"world","user_id":"90001"}
POST /chat/publish      {"channel":"custom","user_id":"90001","text":"hello","message_type":"text"}
POST /chat/world/publish   {"user_id":"90001","text":"hello"}
POST /chat/group/publish   {"group_id":"100","user_id":"90001","text":"hello"}
POST /chat/private/publish {"from_user_id":"90001","to_user_id":"90002","text":"hello"}
POST /chat/recall       {"channel":"world","message_id":"1"}
GET  /chat/messages?channel=world&limit=50
GET  /chat/world/messages?limit=50
GET  /chat/group/messages?group_id=100&limit=50
GET  /chat/private/messages?user_id=90001&peer_user_id=90002&limit=50
POST /chat/friend/add    {"user_id":"90001","friend_user_id":"90002"}
POST /chat/friend/remove {"user_id":"90001","friend_user_id":"90002"}
GET  /chat/friend/list?user_id=90001
```

chat web 内部分层：`app` 只负责 HTTP server 生命周期和依赖注入；`handler` 只做 HTTP/JSON 适配；`service` 负责会话类型、消息类型、好友关系、Redis 存储和 pub/sub 推送事件。当前会话类型包括通用 channel、世界、群聊、私聊；消息类型先支持 `text/image/voice/system/custom`。

world 监听 HTTP，给外部 web 使用：

```text
GET /game/login_options?player_uid=<uid>
```

成功返回 JSON：

```json
{
  "ok": true,
  "gateways": [
    { "host": "127.0.0.1", "port": 30001, "name": "gateway-a" }
  ],
  "roles": [
    { "role_id": 10001, "name": "knight", "level": 12 }
  ],
  "zones": [
    { "name": "zone-a", "online_players": 12, "max_players": 1000, "available": true }
  ]
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
  - gateway 内部 session id 分配。
  - invalid connection context 拒绝。
  - 裸客户端 payload 通过 gateway 按 connection context 转发到 zone。
  - zone 登录后把 `gateway_session_id` 绑定到 role，用于后续 push/cleanup。
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

- `test_game_server_world_db_proxy.cpp`
  - `world_db.ownership.compare_and_set` 通过 proxy 执行 CAS。
  - 验证新登录替换 owner、旧 logout 被拒绝、当前 logout 清理 owner。
  - Redis 不可用时自跳过。

常用验证命令：

```bash
cmake --build build --target game_global_server game_zone_server game_world_server game_gateway_server game_mock_client test_game_server_smoke test_game_server_login_flow test_game_server_tunnel test_game_server_client_frame test_game_server_world_ownership test_game_server_world_db_proxy
ctest --test-dir build -R "game_server_world_db_proxy|game_server_client_frame|game_server_world_ownership|game_server_login_flow|game_server_tunnel|game_server_smoke" --output-on-failure -V
```

本地真实服务启动脚本：

```bash
libs/game/server/scripts/start_all.sh
libs/game/server/scripts/stop_all.sh
```

Windows PowerShell：

```powershell
libs/game/server/scripts/start_all.ps1
libs/game/server/scripts/stop_all.ps1
```

脚本默认使用 `build/bin` 和 `build/libs/game/server/config`，pid/log 写到 `build/game_server_run`；可用 `BUILD_DIR` 或 `RUN_DIR` 环境变量覆盖。

启动/停止顺序有依赖要求：

- `start_all.sh`：先启动 tunnel，再启动 `player_db_proxy/world_db_proxy/global_db_proxy`，最后启动业务进程。
- `stop_all.sh`：先给外部入口和业务进程发 `SIGTERM`，等待它们正式停服和 flush，再关闭 db proxy，最后关闭 tunnel。
- `stop_all.sh` 默认不使用 `SIGKILL`。如果某个进程超过 `STOP_GRACE_SECONDS` 仍未退出，脚本只报错并保留进程，避免破坏 graceful shutdown 和最终落库。
- `start_all.ps1` / `stop_all.ps1` 保持同样的启停顺序。`stop_all.ps1` 默认只尝试 graceful close；如果进程没有可关闭主窗口，会保留进程并报错。只有显式传 `-Force` 时才会强制结束进程。

## 当前已知边界

当前实现已经具备本地多进程骨架、gateway 持久 TCP/YuanRpc 收包路径、gateway->zone 持久连接复用和 request_id response demux、connection-id session 绑定、push 写回连接、断线 cleanup、连接数/缓冲/idle 控制、zone call pending 队列上限和 drain 关闭连接基础能力，但还不是完整商业长连接网关。需要继续补齐的方向：

- gateway 连接生命周期细化：认证前超时、慢客户端分级处理、按 IP/账号维度限额。
- gateway push 写入的背压处理、失败重试/丢弃策略和客户端 push ack。
- 断线 cleanup 的重试与补偿：zone/world logout 失败时的可靠队列或延迟重试。
- 客户端协议 fuzz 测试、rate limit、replay window 策略、packet-size 策略细化。
- Prometheus exporter。
- 更完整 graceful shutdown/drain：跨服务 pending request 可观测等待、超时关闭、flush session、push ack/flush。
- 多 world 部署下的 ownership proxy/world_db CAS 长时间运行测试。
- 更完整的 Redis role/account/player schema。

这些边界不是设计缺陷，而是下一阶段生产化工作的明确接缝。
