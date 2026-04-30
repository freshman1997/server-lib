# SOCKS5 协议实现设计文档

## 1. 概述

本文档描述基于现有 `server-lib` 架构新增 SOCKS5 代理协议的实现设计。SOCKS5（RFC 1928）是一种应用层代理协议，支持 TCP 连接代理和用户认证（RFC 1929），可作为网络代理、内网穿透、流量转发等场景的基础组件。

## 2. 设计目标

- 完全遵循 RFC 1928（SOCKS5）和 RFC 1929（用户名/密码认证）
- 适配现有 `protocol/` 目录下的协议实现模式（参考 HttpServer / WebSocketServer）
- 适配现有 `server/services/` 目录下的 Service 包装模式
- 支持 CONNECT 命令（BIND / UDP ASSOCIATE 预留接口）
- 支持 No Auth 和 Username/Password 两种认证方式
- 可扩展的 `Socks5Handler` 回调接口，允许上层自定义认证和连接控制逻辑

## 3. 架构设计

### 3.1 整体分层

```
┌─────────────────────────────────────────────┐
│              Application (main.cpp)          │
│   add_typed_service<Socks5Service>(...)      │
├─────────────────────────────────────────────┤
│           Service Layer                      │
│   Socks5Service : Service                    │
│                 : RuntimeContextAwareService  │
│   - 持有 Socks5Server (unique_ptr)           │
│   - 独立线程运行 event_loop_->loop()          │
│   - 通过 EventBus 发布生命周期事件             │
├─────────────────────────────────────────────┤
│           Protocol Layer                     │
│   Socks5Server : ConnectionHandler           │
│                 : ConnectorHandler            │
│   - 管理 Poller / EventLoop / Acceptor       │
│   - 管理 Socks5Session 生命周期               │
│   - 协议状态机驱动                            │
│   - 数据双向中继                              │
├─────────────────────────────────────────────┤
│           Core Layer                         │
│   Poller / EventLoop / Acceptor / Connection │
│   TcpConnector / ByteBuffer / TimerManager   │
└─────────────────────────────────────────────┘
```

### 3.2 类图

```
┌──────────────────────────┐
│     Socks5ServerConfig   │
│  - enable_auth           │
│  - username / password   │
│  - enable_connect        │
│  - enable_bind           │
│  - enable_udp_associate  │
│  - connect_timeout_ms    │
│  - idle_timeout_ms       │
│  - max_connections       │
└──────────────────────────┘
          │
          ▼
┌──────────────────────────────────────┐
│         Socks5Server                 │
│  : ConnectionHandler                 │
│  : ConnectorHandler                  │
│──────────────────────────────────────│
│  - poller_ / event_loop_ / acceptor_ │
│  - timer_manager_                    │
│  - sessions_: map<Conn*, Session>    │
│  - remote_to_client_: map<Conn*,*>   │
│  - udp_associations_: map<Conn*,     │
│        unique_ptr<UdpAssociation>>   │
│  - udp_conn_to_client_: map<Conn*,*> │
│  - handler_: Socks5Handler*          │
│──────────────────────────────────────│
│  + init(port) / serve() / stop()     │
│  + on_connected/on_read/on_close     │
│  + on_connect_failed/success/timeout │
│  - handle_greeting/auth/request      │
│  - handle_connect                    │
│  - handle_udp_associate              │
│  - on_udp_datagram                   │
│  - forward_udp_to_target/client      │
│  - close_udp_association             │
│  - relay_data / close_session        │
└──────────────────────────────────────┘
          │ 1..*
          ▼
┌──────────────────────────────┐
│       Socks5Session          │
│──────────────────────────────│
│  - client_conn_              │
│  - remote_conn_              │
│  - state_: State             │
│    greeting / auth / request │
│    connecting /              │
│    udp_associate /           │
│    established / closed      │
│  - target_host_ / port_      │
│  - command_ / atyp_          │
│  - username_ / password_     │
└──────────────────────────────┘

┌──────────────────────────────┐
│       Socks5Handler          │
│  <<interface>>               │
│──────────────────────────────│
│  + on_authenticate()         │
│  + on_connect_request()      │
│  + on_session_opened()       │
│  + on_session_closed()       │
└──────────────────────────────┘

┌──────────────────────────────┐
│     Socks5PacketParser       │
│  <<static utility>>          │
│──────────────────────────────│
│  + parse_greeting()          │
│  + parse_auth_request()      │
│  + parse_request()           │
│  + parse_udp_header()        │
│  + build_method_select_reply │
│  + build_auth_reply()        │
│  + build_reply()             │
│  + build_udp_header()        │
└──────────────────────────────┘

┌──────────────────────────────┐
│       Socks5Service          │
│  : Service                   │
│  : RuntimeContextAwareService│
│──────────────────────────────│
│  - port_                     │
│  - server_: unique_ptr       │
│  - handler_: Socks5Handler*  │
│  - started_ / worker_        │
│──────────────────────────────│
│  + init() / start() / stop() │
│  + set_handler()             │
└──────────────────────────────┘
```

## 4. 协议状态机

SOCKS5 协议交互是一个有限状态机，由 `Socks5Session::State` 驱动：

```
  Client                         Server
  ──────                         ──────
  │  Greeting (VER, NMETHODS,    │
  │           METHODS)           │
  │─────────────────────────────>│  State: greeting
  │                              │  → 选择认证方式
  │  Method Selection            │
  │  (VER, METHOD)               │
  │<─────────────────────────────│
  │                              │
  │  [if Username/Password]      │
  │  Auth (ULEN, UNAME, PLEN,    │
  │        PASSWD)               │
  │─────────────────────────────>│  State: auth
  │                              │  → 验证凭据
  │  Auth Reply (VER, STATUS)    │
  │<─────────────────────────────│
  │                              │
  │  Request (VER, CMD, RSV,     │
  │           ATYP, DST.ADDR,    │
  │           DST.PORT)          │
  │─────────────────────────────>│  State: request
  │                              │
  │  [CMD=CONNECT]               │
  │                              │  → 发起到目标连接
  │                              │  State: connecting
  │                              │  → 连接成功
  │  Reply (VER, REP, RSV,       │
  │         ATYP, BND.ADDR,      │
  │         BND.PORT)            │
  │<─────────────────────────────│
  │                              │  State: established
  │  <===== DATA RELAY =====>   │
  │                              │
  │  [CMD=UDP ASSOCIATE]         │
  │                              │  → 创建 UDP relay socket
  │                              │  State: udp_associate
  │  Reply (VER, REP, RSV,       │
  │         ATYP, BND.ADDR,      │
  │         BND.PORT=relay_port) │
  │<─────────────────────────────│
  │                              │
  │  UDP Datagrams:              │
  │  Client ──[SOCKS5 UDP]──>   │
  │  Server ──[raw UDP]──> Target│
  │  Target ──[raw UDP]──>       │
  │  Server ──[SOCKS5 UDP]──>   │
  │  Client                     │
  │                              │
  │  TCP keep-alive only         │
  │  (client data ignored)       │
  │                              │
  │  (TCP连接关闭)               │  State: closed
  │  → 关闭 UDP association      │
```

### 状态转换表

| 当前状态 | 事件 | 动作 | 下一状态 |
|---------|------|------|---------|
| greeting | on_read | 解析 Greeting，选择 Method，发送 Method Select | auth / request / closed |
| auth | on_read | 解析认证请求，验证凭据，发送 Auth Reply | request / closed |
| request | on_read (CMD=CONNECT) | 解析请求，发起远程连接 | connecting / closed |
| request | on_read (CMD=UDP ASSOCIATE) | 创建 DatagramAcceptor，发送 Reply | udp_associate / closed |
| connecting | on_connected_success | 发送成功 Reply | established |
| connecting | on_connect_failed | 发送失败 Reply | closed |
| connecting | on_connect_timeout | 发送 TTL Expired Reply | closed |
| established | on_read (client) | 中继数据到远程连接 | established |
| established | on_read (remote) | 中继数据到客户端连接 | established |
| established | on_close | 关闭对端连接 | closed |
| udp_associate | on_read (TCP) | 清空输入缓冲区（TCP仅维持） | udp_associate |
| udp_associate | on_read (UDP) | 解析UDP Header，转发到目标 | udp_associate |
| udp_associate | on_close (TCP) | 关闭 UDP association | closed |

## 5. 核心流程

### 5.1 初始化流程

```
Socks5Server::init(port)
  ├── new Socket("", port)
  ├── socket->set_none_block / reuse / no_delay / keep_alive
  ├── socket->bind()
  ├── create_stream_acceptor(sock)
  ├── acceptor->listen()
  ├── create_default_poller()   // epoll on Linux, select on others
  ├── new WheelTimerManager
  ├── new EventLoop(poller, timer_manager)
  ├── acceptor->set_connection_handler(this)
  └── acceptor->set_event_handler(event_loop)
```

### 5.2 CONNECT 命令处理流程

```
handle_request(session, conn)
  ├── 解析 SOCKS5 请求 (VER, CMD, ATYP, DST, PORT)
  ├── 验证 CMD 是否允许 (config_.enable_connect)
  ├── handle_connect(session)
  │   ├── 调用 handler_->on_connect_request() (可选拦截)
  │   ├── new TcpConnector
  │   ├── connector->set_data(timer_manager, this, event_loop)
  │   └── connector->connect(target_addr, timeout)
  │
  ├── [连接成功] on_connected_success(remote_conn)
  │   ├── session->set_state(established)
  │   ├── remote_to_client_[remote] = client
  │   ├── send_reply(succeeded)
  │   └── handler_->on_session_opened(session)
  │
  ├── [连接失败] on_connect_failed(remote_conn)
  │   └── send_reply(connection_refused) + close
  │
  └── [连接超时] on_connect_timeout(remote_conn)
      └── send_reply(ttl_expired) + close
```

### 5.3 数据中继流程

当 Session 进入 `established` 状态后，数据通过 `on_read` 事件驱动双向中继：

```
on_read(conn)
  ├── 判断 conn 是 client 还是 remote (via remote_to_client_ map)
  ├── src = conn (可读端)
  ├── dst = remote_to_client_[conn] 或 session->remote_connection()
  ├── buf = src->take_input_byte_buffer()
  └── dst->write_and_flush(buf)
```

### 5.4 UDP ASSOCIATE 命令处理流程

```
handle_request(session, conn) [CMD=UDP ASSOCIATE]
  ├── 验证 CMD 是否允许 (config_.enable_udp_associate)
  ├── handle_udp_associate(session, conn)
  │   ├── handler_->on_connect_request() (可选拦截)
  │   ├── new Socket("", 0, true)  // 创建 UDP socket，端口随机分配
  │   ├── socket->bind()           // 绑定到随机端口
  │   ├── create_datagram_acceptor(sock, timer_manager_)
  │   ├── udp_acceptor->listen()
  │   ├── udp_acceptor->set_event_handler(event_loop_)
  │   ├── udp_acceptor->set_connection_handler(this)
  │   ├── 创建 UdpAssociation { client_conn, udp_acceptor, client_udp_addr }
  │   ├── session->set_state(udp_associate)
  │   ├── send_reply(succeeded, BND.ADDR=0.0.0.0, BND.PORT=relay_port)
  │   └── udp_associations_[client_conn] = assoc
  │
  ├── [收到 UDP 数据报] on_udp_datagram(conn)
  │   ├── 查找 UdpAssociation (via udp_conn_to_client_ / udp_associations_)
  │   ├── 记录客户端 UDP 地址 (首次来源)
  │   ├── Socks5PacketParser::parse_udp_header(buf)
  │   ├── 验证 fragment==0 (不支持分片)
  │   ├── 计算并剥离 SOCKS5 UDP Header
  │   └── forward_udp_to_target(assoc, header, payload)
  │       ├── InetAddress target_addr(header.address, header.port)
  │       └── udp_acceptor->send_datagram(target_addr, payload)
  │
  ├── [目标回复到达] on_read(udp_conn)
  │   └── forward_udp_to_client(assoc, target_addr, payload)
  │       ├── Socks5PacketParser::build_udp_header(atyp, target_ip, target_port)
  │       ├── datagram = udp_header + payload
  │       └── udp_acceptor->send_datagram(client_udp_addr, datagram)
  │
  ├── [TCP 连接数据] on_read(client_tcp_conn)
  │   └── conn->clear_input_buffer()  // TCP 仅维持连接，忽略数据
  │
  └── [TCP 连接关闭] on_close(client_tcp_conn)
      └── close_udp_association(client_conn)
          ├── udp_acceptor->close()
          ├── idle_timer->cancel()
          ├── 清理 udp_conn_to_client_ 映射
          └── udp_associations_.erase(client_conn)
```

#### UDP ASSOCIATE 关键设计

1. **双通道架构**：TCP 连接维持 UDP 关联生命周期，UDP socket 负责实际数据转发。TCP 关闭则 UDP 关联自动终止（RFC 1928 要求）。

2. **UdpAssociation 结构体**：
   - `client_conn`：客户端 TCP 连接，用于生命周期管理
   - `udp_acceptor`：`DatagramAcceptor` 实例，用于收发 UDP 数据报
   - `client_udp_addr`：客户端 UDP 来源地址（首次收到数据报时记录）
   - `idle_timer`：空闲超时定时器（预留）

3. **客户端 UDP 地址学习**：RFC 1928 规定客户端的 UDP 请求来源地址可能与 TCP 连接地址不同。服务器在首次收到 UDP 数据报时记录客户端地址，后续回复发往该地址。

4. **UDP Header 剥离/封装**：
   - 客户端 → 目标：剥离 SOCKS5 UDP Header（RSV + FRAG + ATYP + DST.ADDR + DST.PORT），仅转发 payload
   - 目标 → 客户端：在 payload 前封装 SOCKS5 UDP Header

5. **分片不支持**：`fragment != 0` 的数据报被丢弃（大多数实现不支持 UDP 分片）

6. **与 DnsServer 的 UDP 模式对比**：
   - DnsServer 使用单一 `DatagramAcceptor` 处理所有 DNS 查询
   - Socks5Server 为每个 UDP ASSOCIATE 创建独立的 `DatagramAcceptor`，绑定随机端口
   - 两者共享同一个 `EventLoop` 和 `Poller`

## 6. 文件结构

```
protocol/socks5/
├── CMakeLists.txt
├── include/
│   ├── socks5.h                    # 统一头文件
│   ├── socks5_protocol.h           # 协议常量、枚举、结构体定义
│   ├── socks5_config.h             # Socks5ServerConfig
│   ├── socks5_handler.h            # Socks5Handler 回调接口
│   ├── socks5_session.h            # Socks5Session 会话状态
│   ├── socks5_packet_parser.h      # 报文解析/构建工具
│   └── socks5_server.h             # Socks5Server 核心服务器
└── src/
    ├── socks5_server.cpp           # 服务器实现
    ├── socks5_session.cpp          # 会话实现
    └── socks5_packet_parser.cpp    # 报文解析实现

server/services/
├── include/
│   └── socks5_service.h            # Socks5Service 服务包装
└── src/
    └── socks5_service.cpp          # 服务包装实现
```

## 7. 构建系统集成

### 7.1 新增 CMake 目标

- `Socks5Proto`：SOCKS5 协议库，链接 `Core`
- `ServerServices`：新增依赖 `Socks5Proto`

### 7.2 修改的 CMakeLists.txt

| 文件 | 变更 |
|------|------|
| `CMakeLists.txt` | 新增 `add_subdirectory(protocol/socks5)`，Test 链接 `Socks5Proto` |
| `protocol/socks5/CMakeLists.txt` | 新建，定义 `Socks5Proto` 库 |
| `server/services/CMakeLists.txt` | `target_link_libraries` 新增 `Socks5Proto` |

## 8. 服务注册

在 `main.cpp` 中注册 SOCKS5 服务：

```cpp
#include "socks5_service.h"

yuan::net::socks5::Socks5ServerConfig socks5_config;
socks5_config.enable_auth = false;
socks5_config.enable_connect = true;

application.add_typed_service<yuan::server::Socks5Service>(
    "socks5",
    std::make_shared<yuan::server::Socks5Service>(1080, socks5_config),
    "server.socks5",
    1);
```

## 9. 可扩展性设计

### 9.1 Socks5Handler 接口

```cpp
class Socks5Handler {
    virtual bool on_authenticate(const std::string &username, const std::string &password) = 0;
    virtual bool on_connect_request(Socks5Session *session, const std::string &host, uint16_t port) = 0;
    virtual void on_session_opened(Socks5Session *session) = 0;
    virtual void on_session_closed(Socks5Session *session) = 0;
};
```

- `on_authenticate`：自定义认证逻辑（数据库查询、LDAP 等）
- `on_connect_request`：访问控制（ACL、IP 黑白名单、域名过滤）
- `on_session_opened/closed`：会话统计、日志审计

### 9.2 预留扩展点

| 功能 | 状态 | 说明 |
|------|------|------|
| CONNECT 命令 | 已实现 | TCP 代理连接 |
| BIND 命令 | 预留 | 反向连接，FTP 主动模式等 |
| UDP ASSOCIATE | 已实现 | UDP 代理，使用 DatagramAcceptor |
| GSSAPI 认证 | 预留 | Kerberos 等强认证 |
| SSL/TLS 支持 | 预留 | 复用 SSLModule |
| 流量统计 | 预留 | 通过 Socks5Handler 扩展 |
| 连接限速 | 预留 | 可集成 TokenBucket |

## 10. 与现有协议实现的对比

| 特性 | HttpServer | WebSocketServer | Socks5Server |
|------|-----------|-----------------|-------------|
| 基类 | ConnectionHandler | 内部 ServerData 继承 ConnectionHandler | ConnectionHandler + ConnectorHandler |
| 运行时 | Poller + EventLoop + TimerManager | Poller + EventLoop + TimerManager | Poller + EventLoop + TimerManager |
| 服务包装 | HttpService | WebSocketService | Socks5Service |
| 会话管理 | HttpSession (unordered_map) | WebSocketConnection (unordered_map) | Socks5Session (unordered_map) |
| 特殊之处 | 请求分发/路由/中间件 | 握手升级/帧协议/心跳 | 双向中继/远程连接/TcpConnector |
| 报文解析 | HttpRequestParser (状态机) | WebSocketPacketParser | Socks5PacketParser (静态工具) |

## 11. 关键设计决策

### 11.1 Socks5Server 同时实现 ConnectionHandler 和 ConnectorHandler

- `ConnectionHandler`：处理客户端连接的 on_connected/on_read/on_close
- `ConnectorHandler`：处理到远程目标的连接结果回调
- 两者在同一个 EventLoop 中运行，避免跨线程问题

### 11.2 remote_to_client_ 映射表

当远程连接建立后，远程连接的 on_read 事件需要找到对应的客户端连接来转发数据。`remote_to_client_` 映射表实现了从远程连接到客户端的反向查找。

### 11.3 零拷贝中继

`relay_data()` 使用 `take_input_byte_buffer()` 取走源连接的输入缓冲区（移动语义），直接通过 `write_and_flush()` 写入目标连接，避免数据拷贝。

### 11.4 Socks5Handler 回调 vs 配置认证

当设置了 `Socks5Handler` 时，认证逻辑优先使用 `handler_->on_authenticate()`；否则回退到配置文件中的 `config_.username/password` 比对。这提供了灵活性和开箱即用的双模式。

## 12. 测试

### 12.1 自动化测试（test_socks5.cpp）

跨平台（Win/Linux/macOS）测试套件，包含：

**单元测试**：
- 协议枚举值验证
- Greeting 解析：No Auth / 多方法 / 不完整 / 错误版本 / 空 / 最大方法数
- Auth 请求解析：正常 / 不完整
- Request 解析：CONNECT+IPv4 / CONNECT+Domain / CONNECT+IPv6 / UDP ASSOCIATE / 不完整
- Method Select Reply 构建
- Auth Reply 构建
- Reply 构建：IPv4 / Domain / IPv6 / 默认值 / 往返验证 / 所有错误码
- UDP Header 解析：IPv4 / Domain / IPv6 / 不完整 / 空 / 分片字段 / payload 分离
- UDP Header 构建：IPv4 / Domain / IPv6
- UDP Header 往返验证：IPv4 / Domain / IPv6
- Config 默认值 / 自定义值
- Session 状态枚举顺序

**服务器对象测试**：
- Socks5Server 初始化 / 带认证配置 / Handler 设置

**E2E 测试**：
- TCP CONNECT 握手（No Auth）
- TCP CONNECT 握手（Username/Password Auth）
- UDP ASSOCIATE 完整流程（TCP 协商 + UDP 数据报发送）

### 12.2 手动测试

```bash
# 使用 curl 测试 SOCKS5 代理
curl --socks5 127.0.0.1:1080 http://example.com

# 使用 curl 测试带认证的 SOCKS5 代理
curl --socks5 --proxy-user user:pass 127.0.0.1:1080 http://example.com
```

### 12.3 测试构建

```bash
cd build
cmake .. -DBUILD_TESTING=ON
cmake --build .
ctest -R socks5
```
