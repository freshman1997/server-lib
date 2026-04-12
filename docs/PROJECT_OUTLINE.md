# 项目大纲

## 1. 项目定位

这是一个基于 `C++20 + CMake` 的网络服务端/协议库项目，核心目标是提供一套可复用的底层网络能力，并在其上实现多个常见协议模块。

从当前代码结构看，项目主要由两层组成：

- 底层通用能力：事件循环、poller、socket、连接、缓冲区、定时器、线程、插件等
- 上层协议与扩展：HTTP、FTP、DNS、WebSocket、BitTorrent、日志、Redis 客户端、RPC、插件示例

顶层构建入口会产出多个库、测试程序，以及一个示例可执行文件 `Test`。

## 2. 顶层目录结构

```text
webserver/
|-- core/                 核心基础设施
|-- protocol/             协议实现
|-- libs/                 附加库
|-- logger/               日志模块
|-- plugins/              插件示例
|-- test/                 协议/模块测试程序
|-- third_party/          第三方依赖
|-- build*.sh             Linux/Mingw 构建脚本
|-- CMakeLists.txt        顶层 CMake 入口
|-- main.cpp              示例入口
|-- readme.md             项目说明
|-- ca/                   证书/密钥样例
```

## 3. 构建与产物

顶层 `CMakeLists.txt` 会加入以下子模块：

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

主要库目标：

- `Core`
- `App`
- `HttpProto`
- `FtpProto`
- `DnsProto`
- `BitTorrentProto`
- `WebSocketProto`
- `Logger`

其他目标：

- `HelloWorld.plugin`：插件示例
- `Test`：示例主程序，当前直接启动 `DnsServer`
- `test/` 下多个协议测试可执行程序

可选编译开关：

- `HTTP_USE_SSL`
- `WS_USE_SSL`

顶层还包含 OpenSSL 头文件与库目录配置，说明项目默认依赖本地编译好的 `third_party/openssl-3.4.0`。

## 4. 核心分层

### 4.1 `core/`

`core` 是整个项目的基础层，分为两个子模块：

- `core/core`：底层运行时与网络基础设施
- `core/app`：应用层封装，依赖 `Core` 和 `Logger`

`core/core/include` 下的主要能力目录：

- `api`：对外暴露的核心接口
- `base`：基础工具类与通用能力
- `buffer`：缓冲区实现
- `endian`：字节序相关处理
- `event`：事件循环
- `message`：消息分发
- `net`：网络层核心实现
- `plugin`：插件加载与符号解析
- `singleton`：单例工具
- `thread`：线程与线程池
- `timer`：定时器与时间轮

从源码目录可以进一步看出 `core` 的重点组件：

- `net/poller/`：`select`、`poll`、`epoll`、`kqueue` 适配
- `net/socket/`：socket 与地址封装
- `net/acceptor/`：TCP/UDP 接入
- `net/connection/`：TCP/UDP 连接对象
- `event/`：事件循环驱动
- `buffer/`：链式缓冲区、内存池
- `thread/`：线程、工作线程、线程池
- `timer/`：时间轮定时器
- `plugin/`：插件管理与符号解析

### 4.2 `logger/`

独立日志模块，编译为 `Logger` 库，并依赖 `Core`。

当前结构包含：

- `include/console_logger.h`
- `include/file_logger.h`
- `include/net_logger.h`
- `include/logger_factory.h`
- `include/pipeline.h`
- `include/renderer.h`
- `config/log_cfg.json`

说明它已经具备控制台日志、文件日志、日志工厂和配置的基础形态。

### 4.3 `libs/`

当前包含两个扩展方向：

- `redis_cli/`：Redis 客户端封装，命令、响应值类型、内部命令实现较完整
- `rpc/`：RPC 相关头文件与 DSL 生成脚本雏形，当前更像实验性模块

其中 `libs/CMakeLists.txt` 当前只显式加入了 `redis_cli`，`rpc` 还未接入顶层构建产物。

### 4.4 `plugins/`

当前提供一个 `helloworld` 插件示例，编译后输出为 `.plugin` 动态库文件。

这部分与 `core/plugin` 配合，用于验证插件机制。

## 5. 协议模块

`protocol/` 下当前包含五个协议目录：

- `http`
- `ftp`
- `dns`
- `bit_torrent`
- `websocket`

### 5.1 HTTP

编译目标：`HttpProto`

从源码命名可以看出，HTTP 模块已经覆盖较多功能：

- 报文与包结构：`packet.cpp`、`packet_parser.cpp`
- 请求/响应：`request.cpp`、`response.cpp`
- 解析器：`request_parser.cpp`、`response_parser.cpp`
- 服务端/客户端：`http_server.cpp`、`http_client.cpp`
- 路由/分发：`request_dispatcher.cpp`
- 会话与上下文：`session.cpp`、`context.cpp`
- 头部与鉴权：`header_util.cpp`、`header_key.cpp`、`authorization.cpp`
- 文件任务：`task/upload_file_task.cpp`、`task/download_file_task.cpp`
- 流媒体相关：`media.cpp`
- 配置：`ops/config_manager.cpp`、`ops/http.json`
- 代理：`proxy.cpp`

这是当前仓库里最完整、最接近实际 Web 服务能力的协议模块之一。

### 5.2 FTP

编译目标：`FtpProto`

FTP 模块分层较清晰：

- `include/common` / `src/common`：通用会话、文件流、文件管理
- `include/server` / `src/server`：FTP 服务端与会话管理
- `include/client` / `src/client`：FTP 客户端
- `include/server/commands` / `src/server/commands`：FTP 命令处理器
- `include/handler`：应用层事件接口

说明该模块采用了较典型的 FTP 控制连接/数据连接拆分设计。

### 5.3 DNS

编译目标：`DnsProto`

当前模块相对精简，核心文件包括：

- `dns_server.cpp`
- `dns_client.cpp`
- `dns_packet.h`

适合作为轻量协议示例，也被 `main.cpp` 当前默认用于启动测试服务。

### 5.4 WebSocket

编译目标：`WebSocketProto`

依赖 `Core` 与 `HttpProto`，说明握手流程复用了 HTTP 能力。

目录结构分为：

- `common/`：握手、连接、配置、协议、工具、数据帧解析
- `entry/`：客户端/服务端入口封装
- `include/`：统一对外头文件

从文件命名看，已覆盖：

- 握手
- 帧解析与组包
- 连接管理
- 客户端/服务端
- 关闭码与公共 handler

### 5.5 BitTorrent

编译目标：`BitTorrentProto`

当前更偏实验/基础实现，已有内容包括：

- `bencoding`
- `peer`
- `bit_torrent_client`
- 工具函数

### 5.6 模块关系

大致依赖关系如下：

```text
Core
|-- Logger
|-- App
|-- HttpProto
|-- FtpProto
|-- DnsProto
|-- BitTorrentProto
|-- WebSocketProto -> HttpProto

Core/plugin <-> plugins/helloworld
libs/redis_cli -> Core
```

## 6. 测试与示例入口

`test/CMakeLists.txt` 当前会生成这些测试程序：

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

这些目标说明仓库不仅有库代码，也保留了较多“可直接运行验证”的样例入口。

`main.cpp` 更像开发期实验入口，目前主要做了这些事情：

- 初始化平台相关网络环境
- 保留若干底层测试函数
- 默认创建并启动 `net::dns::DnsServer`

因此如果要快速理解运行链路，建议优先看：

1. `main.cpp`
2. `test/test_http_server.cpp`
3. `test/test_http_client.cpp`
4. `test/test_websocket_server.cpp`
5. `protocol/http/src/http_server.cpp`
6. `core/core/src/event/event_loop.cpp`

## 7. 第三方依赖

目前仓库中能明确看到的第三方依赖有：

- `third_party/kcp`
- `third_party/json`
- `third_party/openssl-3.4.0`

其中：

- `kcp` 已通过 CMake 接入
- `json` 以头文件方式引入
- `openssl-3.4.0` 需要预先构建，并由 `Core`/顶层 CMake 链接

`ca/` 目录下包含证书与密钥样例，和 HTTP/WebSocket 的 SSL 开关相呼应。

## 8. 当前项目特征总结

从仓库状态看，这个项目有几个明显特征：

- 以网络底层抽象为核心，而不是只做单一协议
- HTTP、FTP、WebSocket 已经具备较多可运行代码
- DNS、BitTorrent、RPC 更像继续扩展中的模块
- 测试目录承担了相当一部分“样例程序”职责
- 插件机制、日志、Redis 客户端等能力已经开始围绕核心框架扩展

## 9. 建议的阅读顺序

如果是第一次接手，建议按下面顺序阅读：

1. 顶层 `CMakeLists.txt`：先理解构建目标
2. `core/core/include` 与 `core/core/src/event`、`net`：理解底层运行模型
3. `protocol/http`：理解最完整的协议实现
4. `protocol/websocket`：理解在 HTTP 基础上的扩展
5. `protocol/ftp`：理解另一套协议处理组织方式
6. `test/`：看实际使用方式
7. `plugins/` 和 `libs/`：看扩展能力

## 10. 可继续补充的文档方向

如果后面要继续完善文档，建议再拆出几份专题文档：

- `docs/BUILD_GUIDE.md`：整理实际构建步骤与平台差异
- `docs/CORE_ARCHITECTURE.md`：讲清楚事件循环、连接、poller、timer 的关系
- `docs/HTTP_FLOW.md`：梳理 HTTP 请求进入后的完整处理链路
- `docs/PLUGIN_SYSTEM.md`：说明插件加载机制
- `docs/TEST_ENTRY_MAP.md`：列出每个测试程序对应验证的能力

