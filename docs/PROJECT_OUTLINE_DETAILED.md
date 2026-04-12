# 当前项目详细大纲

## 1. 一句话概览

这是一个以 `Core` 网络运行时为基础、面向多协议扩展的 `C++20 + CMake` 项目。仓库里不仅有底层网络抽象，还实现了 `HTTP`、`FTP`、`DNS`、`WebSocket`、`BitTorrent`，并扩展了日志、Redis 客户端、插件机制与若干测试入口。

如果把它看成一套分层系统，可以粗略理解为：

```text
应用/测试入口
    ↓
协议层: HTTP / FTP / DNS / WebSocket / BitTorrent
    ↓
基础设施层: EventLoop / Poller / Socket / Connection / Buffer / Timer / Thread
    ↓
第三方依赖: OpenSSL / KCP / JSON
```

## 2. 仓库总览

### 2.1 顶层目录地图

```text
webserver/
|-- core/                         核心运行时与应用封装
|-- protocol/                     协议实现
|-- libs/                         扩展库
|-- logger/                       日志库
|-- plugins/                      插件示例
|-- test/                         测试与样例程序
|-- third_party/                  第三方代码
|-- ca/                           证书与密钥样例
|-- build/                        构建输出目录
|-- cmake-build-*/                IDE/CMake 生成目录
|-- build.sh                      Linux/Mingw 构建脚本
|-- build_openssl.sh              OpenSSL 构建脚本
|-- build_release.sh              Release 构建脚本
|-- CMakeLists.txt                顶层构建入口
|-- main.cpp                      示例程序入口
|-- readme.md                     项目说明
|-- filelist.html                 静态页面样例
|-- upload.html                   上传页面样例
```

### 2.2 顶层 CMake 实际接入内容

顶层构建会加入这些模块：

- `third_party/kcp`
- `core`
- `protocol/http`
- `protocol/ftp`
- `protocol/dns`
- `protocol/bit_torrent`
- `protocol/websocket`
- `test`
- `plugins`
- `logger`
- `libs`

会生成的核心库包括：

- `Core`
- `App`
- `HttpProto`
- `FtpProto`
- `DnsProto`
- `BitTorrentProto`
- `WebSocketProto`
- `Logger`

此外还会生成：

- `HelloWorld.plugin`
- `Test`
- `test/` 下多个样例程序

### 2.3 编译相关特征

- 使用 `C++20`
- 全局启用 `POSITION_INDEPENDENT_CODE`
- 提供 `HTTP_USE_SSL`、`WS_USE_SSL` 开关
- 依赖本地已构建的 `third_party/openssl-3.4.0`
- `Core` 在不同平台上链接 `ssl/crypto`、`ws2_32`、`crypt32`、`pthread` 等平台库

## 3. 模块分层

### 3.1 第一层：Core 基础设施

`core/` 是整个项目最关键的一层，它决定了协议模块的运行方式。

目录结构：

```text
core/
|-- CMakeLists.txt
|-- core/
|   |-- CMakeLists.txt
|   |-- include/
|   |   |-- api/
|   |   |-- base/
|   |   |-- buffer/
|   |   |-- endian/
|   |   |-- event/
|   |   |-- message/
|   |   |-- net/
|   |   |-- plugin/
|   |   |-- singleton/
|   |   |-- thread/
|   |   |-- timer/
|   |-- src/
|       |-- base/
|       |-- buffer/
|       |-- event/
|       |-- message/
|       |-- net/
|       |-- plugin/
|       |-- thread/
|       |-- timer/
|-- app/
    |-- CMakeLists.txt
    |-- include/
    |-- src/
```

它可以继续拆成几个子系统。

### 3.2 事件驱动子系统

相关目录：

- `core/core/include/event`
- `core/core/include/net/poller`
- `core/core/include/net/channel`
- `core/core/include/net/handler`
- `core/core/src/event`
- `core/core/src/net/poller`
- `core/core/src/net/channel`

关键对象：

- `EventLoop`
- `Poller`
- `Channel`
- `EventHandler`
- `ConnectionHandler`

当前已经适配的 I/O 多路复用实现：

- `SelectPoller`
- `PollPoller`
- `EpollPoller`
- `KqueuePoller`

这说明项目的整体运行模型是“事件循环驱动”，协议层通常通过 `ConnectionHandler` 接管连接生命周期。

### 3.3 网络连接子系统

相关目录：

- `core/core/include/net/socket`
- `core/core/include/net/acceptor`
- `core/core/include/net/connection`
- `core/core/include/net/connector`
- `core/core/src/net/socket`
- `core/core/src/net/acceptor`
- `core/core/src/net/connection`
- `core/core/src/net/connector`

关键职责：

- `Socket`：底层 socket 与地址封装
- `TcpAcceptor` / `UdpAcceptor`：监听与接入
- `TcpConnection` / `UdpConnection`：连接对象
- `TcpConnector`：主动连接方封装
- `InetAddress`：地址抽象

从 `TcpAcceptor` 的接口可以看出，它负责：

- 创建监听通道
- 接收新连接
- 接入 `EventHandler`
- 接入 `ConnectionHandler`
- 在需要时挂接 SSL 模块

### 3.4 缓冲区与数据承载

相关目录：

- `core/core/include/buffer`
- `core/core/src/buffer`

当前可以看到的实现：

- `linked_buffer.cpp`
- `pool.cpp`

这表明项目不是直接用裸 `std::string`/`vector<char>` 处理所有收发，而是有一套自己的缓冲区层，这对协议解析器尤其重要。

### 3.5 定时器与线程

相关目录：

- `core/core/include/timer`
- `core/core/src/timer`
- `core/core/include/thread`
- `core/core/src/thread`

主要对象：

- `Timer`
- `TimerTask`
- `TimerManager`
- `WheelTimer`
- `WheelTimerManager`
- `Thread`
- `WorkerThread`
- `ThreadPool`
- `Runnable`

说明：

- 定时器部分采用了时间轮方向的实现
- 线程层既支持单任务线程，也支持线程池
- 适合协议层做超时控制、后台任务、文件处理等

### 3.6 插件与消息

相关目录：

- `core/core/include/plugin`
- `core/core/src/plugin`
- `core/core/src/message`

插件相关文件：

- `plugin_manager.cpp`
- `plugin_symbol_solver.cpp`

这部分与 `plugins/helloworld` 配合，说明仓库支持以动态库形式扩展功能。

## 4. 应用封装层：`core/app`

`App` 模块依赖：

- `Core`
- `Logger`

可见它的定位不是底层 I/O 本身，而是比 `Core` 更贴近业务应用的一层薄封装。目前该层代码量不大，但适合作为后续公共服务层的放置位置。

## 5. 协议层总览

### 5.1 协议目录树

```text
protocol/
|-- http/
|-- ftp/
|-- dns/
|-- bit_torrent/
|-- websocket/
```

这些协议模块几乎都直接或间接依赖 `Core`，其中 `WebSocketProto` 额外依赖 `HttpProto`。

## 6. HTTP 模块详解

### 6.1 目录结构

```text
protocol/http/
|-- CMakeLists.txt
|-- include/
|-- src/
|   |-- content/
|   |-- ops/
|   |-- task/
|   |-- authorization.cpp
|   |-- context.cpp
|   |-- content_type.cpp
|   |-- header_key.cpp
|   |-- header_util.cpp
|   |-- http_client.cpp
|   |-- http_server.cpp
|   |-- media.cpp
|   |-- packet.cpp
|   |-- packet_parser.cpp
|   |-- proxy.cpp
|   |-- request.cpp
|   |-- request_dispatcher.cpp
|   |-- request_parser.cpp
|   |-- response.cpp
|   |-- response_code_desc.cpp
|   |-- response_parser.cpp
|   |-- session.cpp
|   |-- url.cpp
```

### 6.2 模块职责划分

可以把 HTTP 代码分成六组：

1. 报文模型

- `packet.*`
- `request.*`
- `response.*`

2. 报文解析

- `packet_parser.cpp`
- `request_parser.cpp`
- `response_parser.cpp`

3. 服务端/客户端

- `http_server.cpp`
- `http_client.cpp`

4. 请求处理

- `request_dispatcher.cpp`
- `session.cpp`
- `context.cpp`

5. 辅助能力

- `header_util.cpp`
- `header_key.cpp`
- `content_type.cpp`
- `response_code_desc.cpp`
- `authorization.cpp`
- `url.cpp`

6. 文件/媒体/配置

- `task/upload_file_task.cpp`
- `task/save_upload_tmp_chunk_task.cpp`
- `task/download_file_task.cpp`
- `media.cpp`
- `ops/config_manager.cpp`
- `ops/option.cpp`
- `ops/http.json`
- `proxy.cpp`

### 6.3 HTTP 服务端对象职责

`HttpServer` 头文件直接暴露出它的核心责任：

- 作为 `ConnectionHandler` 接管连接生命周期
- `init(port)` 完成端口初始化
- `serve()` 启动事件循环
- `on(url, func, is_prefix)` 注册路由
- 管理 `sessions_`
- 管理 `static_paths_`
- 管理上传分片 `uploaded_chunks_`
- 持有 `ThreadPool`
- 可挂接 SSL 模块
- 内置下载、目录展示、静态文件、配置重载、上传处理

这意味着 `HttpServer` 不是一个极薄的 socket 包装，而是已经带一定 Web 服务能力的入口对象。

### 6.4 HTTP 解析链路

从文件关系推断，典型请求链路大致是：

```text
TcpConnection 收到数据
    ↓
HttpServer::on_read
    ↓
HttpSession / HttpSessionContext
    ↓
HttpPacketParser / HttpRequestParser
    ↓
HttpRequest 对象
    ↓
HttpRequestDispatcher 路由分发
    ↓
HttpResponse 组包回写
```

`packet_parser.h` 显示了解析器内部状态机的设计：

- `HeaderState`
- `BodyState`

说明 HTTP 解析不是一次性切割字符串，而是按状态推进的增量解析模型，这对粘包、半包、上传场景很关键。

### 6.5 HTTP 当前功能判断

基于源码命名，当前 HTTP 模块大概率已覆盖：

- 请求行/响应行解析
- Header 解析
- Body 解析
- 文件上传
- 文件下载
- 静态资源服务
- 流媒体片段输出
- HTTP 客户端
- 基础代理逻辑
- 可选 SSL

这部分是当前仓库里最成熟、最值得优先阅读的协议模块。

## 7. FTP 模块详解

### 7.1 目录结构

```text
protocol/ftp/
|-- CMakeLists.txt
|-- include/
|   |-- client/
|   |-- common/
|   |-- handler/
|   |-- server/
|   |   |-- commands/
|-- src/
    |-- client/
    |-- common/
    |-- server/
        |-- commands/
```

### 7.2 分层理解

FTP 模块的组织比 HTTP 更“协议化”，分层非常明确：

- `common/`：会话、文件流、文件管理、共用定义
- `server/`：服务端会话、命令解析、命令执行、数据连接管理
- `client/`：客户端配置、命令扫描、响应解析、文件流
- `handler/`：上层应用回调接口

### 7.3 服务端职责

`FtpServer` 同时继承：

- `ConnectionHandler`
- `FtpApp`

这说明它既处理网络生命周期，也暴露了一层 FTP 应用上下文接口，用于：

- 获取 `TimerManager`
- 获取事件处理器
- 判断服务状态
- 在 session 关闭时回调
- 触发退出

### 7.4 命令实现现状

当前已能看到不少 FTP 命令处理器文件，例如：

- `user`
- `pasv`
- `retr`
- `stor`
- `list`
- `abort`

同时 `include/server/commands` 中还列出了更多 FTP 命令头文件，说明这里已经在向“命令分发器 + 命令对象”的方式组织。

## 8. DNS 模块详解

### 8.1 目录结构

```text
protocol/dns/
|-- CMakeLists.txt
|-- include/
|-- src/
    |-- dns_client.cpp
    |-- dns_server.cpp
```

### 8.2 当前定位

DNS 模块结构较简单，更像一个轻量协议样例或实验模块。

但它有两个值得注意的点：

- 顶层 `main.cpp` 当前默认启动的是 `DnsServer`
- `test_kcp_cli`、`test_kcp_svr` 链接的是 `DnsProto`

所以它在仓库里不只是占位模块，至少已经承担了一部分真实运行/调试入口。

## 9. WebSocket 模块详解

### 9.1 目录结构

```text
protocol/websocket/
|-- CMakeLists.txt
|-- include/
|   |-- websocket.h
|-- common/
|   |-- close_code.h
|   |-- handler.h
|   |-- handshake.*
|   |-- websocket_config.*
|   |-- websocket_connection.*
|   |-- websocket_packet_parser.*
|   |-- websocket_protocol.h
|   |-- websocket_utils.*
|-- entry/
    |-- client.*
    |-- server.*
    |-- data_handler.h
```

### 9.2 结构特征

`include/websocket.h` 直接聚合了：

- `common/websocket_connection.h`
- `entry/data_handler.h`
- `entry/server.h`
- `entry/client.h`

这说明 WebSocket 模块希望给外部一个统一头文件入口。

### 9.3 与 HTTP 的关系

`WebSocketProto` 链接了 `HttpProto`，说明：

- 握手阶段直接复用 HTTP
- 连接建立后切换到 WebSocket 数据帧协议

所以在阅读 WebSocket 之前，先理解 HTTP 模块会更顺手。

### 9.4 当前功能判断

从命名上看，已经覆盖：

- 握手
- 数据帧解析
- 连接封装
- 配置
- 客户端
- 服务端
- 关闭码

## 10. BitTorrent 模块详解

### 10.1 目录结构

```text
protocol/bit_torrent/
|-- CMakeLists.txt
|-- include/
|   |-- structure/
|-- src/
    |-- structure/
    |-- bit_torrent_client.cpp
    |-- peer.cpp
    |-- utils.cpp
```

### 10.2 当前定位

这是一个偏实验性的协议模块，当前重点在：

- `bencoding`
- `peer`
- `client`
- 基础工具

从顶层看来，它已经可以独立编译成 `BitTorrentProto`，但整体成熟度应该低于 HTTP/FTP/WebSocket。

## 11. 扩展库详解

### 11.1 `logger/`

目录结构：

```text
logger/
|-- CMakeLists.txt
|-- config/
|   |-- log_cfg.json
|-- include/
|   |-- color.h
|   |-- console_logger.h
|   |-- file_logger.h
|   |-- log.h
|   |-- logger_factory.h
|   |-- net_logger.h
|   |-- pipeline.h
|   |-- registry.h
|   |-- renderer.h
|-- src/
    |-- console_logger.cpp
    |-- file_logger.cpp
    |-- logger_factory.cpp
```

定位：

- 独立日志库
- 依赖 `Core`
- 具备控制台日志、文件日志、工厂、渲染/管道等基础结构

### 11.2 `libs/redis_cli`

目录结构：

```text
libs/redis_cli/
|-- CMakeLists.txt
|-- src/
    |-- cmd/
    |-- internal/
    |   |-- cmd_impl/
    |-- value/
    |-- redis_client.*
    |-- redis_cli_manager.*
    |-- redis_value.h
    |-- command.h
    |-- option.h
|-- test/
```

定位：

- Redis 客户端封装
- 命令实现分层较细
- 包含值类型抽象，如 `string/int/array/map/error/status/float`
- 看起来已经是比较完整的独立子模块

### 11.3 `libs/rpc`

目录结构更轻，主要是：

- 头文件
- `detail/`
- `dsl/`

其中 `dsl/gen.py`、`hello.json`、`type_mapping.json` 暗示这里在尝试做 DSL 驱动的 RPC 代码生成，但当前 `libs/CMakeLists.txt` 只接入了 `redis_cli`，说明 RPC 尚未完成整体接线。

## 12. 插件机制

### 12.1 目录结构

```text
plugins/
|-- CMakeLists.txt
|-- helloworld/
    |-- helloworld.cpp
    |-- helloworld.h
```

### 12.2 当前产物

插件会被编译成：

- 无前缀
- 后缀为 `.plugin`

当前示例目标名：

- `HelloWorld.plugin`

这和 `core/plugin` 下的 `plugin_manager`、`plugin_symbol_solver` 组成一套最小可用的插件系统。

## 13. 测试与运行入口

### 13.1 `test/` 当前产物

```text
test/
|-- test_http_server.cpp
|-- test_http_client.cpp
|-- test_ftp_server.cpp
|-- test_ftp_client.cpp
|-- test_websocket_server.cpp
|-- test_websocket_client.cpp
|-- test_logger.cpp
|-- test_plugin.cpp
|-- test_udp_kcp_cli.cpp
|-- test_udp_kcp_svr.cpp
```

对应可执行文件：

- `http_server`
- `http_client`
- `ftp_server`
- `ftp_client`
- `ws_svr`
- `ws_cli`
- `test_log`
- `test_plugin`
- `test_kcp_cli`
- `test_kcp_svr`

### 13.2 这些测试的价值

这里的 `test` 更像“样例入口 + 集成验证”，不是严格意义上的单元测试。

它们的价值在于：

- 展示模块的典型用法
- 提供最短运行路径
- 方便本地调试
- 作为理解 API 的最佳样本

### 13.3 `main.cpp` 的角色

顶层 `main.cpp` 当前主要是一个实验入口，包含：

- 平台网络初始化
- 若干注释掉的底层测试代码
- `DnsServer` 的启动示例

它不像最终产品入口，更像开发期总控测试文件。

## 14. 关键依赖关系

可以用下面这张图快速理解主要依赖：

```text
third_party/json
third_party/kcp
third_party/openssl-3.4.0
        ↓
      Core
        ↓
  ┌─────┼───────────────┬────────────┬──────────────┐
  ↓     ↓               ↓            ↓              ↓
Logger App          HttpProto     FtpProto       DnsProto
                      ↓
                  WebSocketProto

BitTorrentProto -> Core + crypto
redis_cli      -> Core
plugins        -> Core
tests          -> 各协议/模块库
```

## 15. 建议阅读顺序

### 15.1 如果你想先理解“底层架构”

按这个顺序读：

1. `CMakeLists.txt`
2. `core/core/include/event/event_loop.h`
3. `core/core/include/net/acceptor/tcp_acceptor.h`
4. `core/core/include/net/connection/*`
5. `core/core/src/event/event_loop.cpp`
6. `core/core/src/net/poller/*`
7. `core/core/src/net/connection/*`
8. `core/core/src/buffer/*`
9. `core/core/src/timer/*`

### 15.2 如果你想先理解“HTTP 服务是怎么跑起来的”

按这个顺序读：

1. `test/test_http_server.cpp`
2. `protocol/http/include/http_server.h`
3. `protocol/http/src/http_server.cpp`
4. `protocol/http/src/session.cpp`
5. `protocol/http/src/packet_parser.cpp`
6. `protocol/http/src/request_parser.cpp`
7. `protocol/http/src/request_dispatcher.cpp`
8. `protocol/http/src/response.cpp`
9. `core/core/src/event/event_loop.cpp`

### 15.3 如果你想快速接手 WebSocket

按这个顺序读：

1. `test/test_websocket_server.cpp`
2. `protocol/websocket/include/websocket.h`
3. `protocol/websocket/entry/server.cpp`
4. `protocol/websocket/common/handshake.cpp`
5. `protocol/websocket/common/websocket_packet_parser.cpp`
6. `protocol/websocket/common/websocket_connection.cpp`
7. `protocol/http` 中与握手相关的基础结构

### 15.4 如果你想研究 FTP

按这个顺序读：

1. `test/test_ftp_server.cpp`
2. `protocol/ftp/include/server/ftp_server.h`
3. `protocol/ftp/src/server/ftp_server.cpp`
4. `protocol/ftp/src/server/server_session.cpp`
5. `protocol/ftp/src/server/command_parser.cpp`
6. `protocol/ftp/src/server/commands/*`
7. `protocol/ftp/src/common/*`

## 16. 当前项目成熟度判断

从仓库结构和命名来看，成熟度大致可以这样理解：

- 完整度较高：`core`、`http`、`ftp`、`websocket`
- 可运行但较轻：`dns`
- 仍在扩展中：`bit_torrent`、`rpc`
- 工具化支撑：`logger`、`redis_cli`、`plugins`

这意味着如果后续要做功能开发，优先在 `HTTP/WebSocket/Core` 这几块继续推进会最稳。

## 17. 建议下一步补文档的方向

如果你准备继续把项目文档系统化，建议拆成下面几份专题文档：

- `docs/CORE_RUNTIME_FLOW.md`
  内容：`Acceptor -> Connection -> Channel -> EventLoop -> Handler` 的完整流转

- `docs/HTTP_REQUEST_FLOW.md`
  内容：HTTP 请求从读 socket 到生成响应的完整调用链

- `docs/HTTP_UPLOAD_FLOW.md`
  内容：上传、分片保存、临时文件、合并/下载链路

- `docs/WEBSOCKET_HANDSHAKE_FLOW.md`
  内容：WebSocket 握手如何复用 HTTP

- `docs/PLUGIN_SYSTEM.md`
  内容：`.plugin` 动态库如何被加载与调度

- `docs/MODULE_INDEX.md`
  内容：每个目录下关键文件一句话说明

## 18. 本文档适合怎么用

这份文档最适合三种场景：

- 第一次接手项目时快速建立全局认识
- 进入某个目录前先判断它在整体中的位置
- 给新同事做代码导览时作为讲解提纲

如果你愿意，我下一步可以直接继续帮你做两份最有价值的配套文档：

1. `HTTP_REQUEST_FLOW.md`，把 HTTP 从连接接入到响应回写完整串起来
2. `MODULE_INDEX.md`，把关键源码文件逐个做一句话索引

