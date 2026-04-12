# 项目整体重构设计文档

## 1. 背景

当前仓库已经具备较多能力：

- `core`：事件循环、连接、buffer、timer、thread、plugin
- `protocol`：HTTP、FTP、DNS、WebSocket、BitTorrent
- `logger`：控制台、文件、网络日志
- `libs`：Redis 客户端、实验性 RPC
- `server`：独立业务服务器
- `test`：大量样例式集成入口

但从整体形态看，仍然存在这些项目级问题：

- 模块边界偏松，协议层、基础设施层、应用层有交叉
- 顶层构建入口职责过重，开关和产物装配方式不统一
- `test/` 同时承担样例、验证、调试入口，职责混杂
- `main.cpp` 更像实验入口，不是稳定产品入口
- 多协议虽然都能编译，但缺少统一运行时模型
- 协程、异步任务、自定义事件、插件扩展点还没有成为统一机制
- 未来 `core/app` 要承接上层应用，但当前还没有形成完整落点

## 2. 重构目标

目标不是把仓库变成“一个巨型库”，而是变成“一套清晰分层、可扩展的网络运行时平台”。

核心目标：

- 现代化：统一到 C++20 风格，减少历史式接口
- 异步化：网络 I/O、定时器、任务调度采用统一异步模型
- 协程化：在不破坏现有能力的前提下，为协程 API 预留接口
- 模块化：协议、扩展库、插件、业务服务器都能按需装配
- 可维护：分层清晰、依赖方向单向、公共能力可复用
- 可扩展：协议可插拔、事件可扩展、插件可独立演进
- 可部署：支持跨平台，支持单进程/多线程，也支持多进程模式扩展

当前平台优先级：

- 第一优先：MinGW
- 第二优先：Linux
- 第三优先：macOS

说明：

- 本轮架构演进不以 MSVC 兼容为阻塞条件
- 平台抽象优先围绕 GCC/Clang 语义与 POSIX/MinGW 共同子集设计

## 3. 目标分层

推荐重构后的项目结构：

```text
webserver/
|-- cmake/                      项目级 CMake 公共配置
|-- docs/                       设计与演进文档
|-- core/
|   |-- runtime/                事件循环、poller、connection、buffer、timer
|   |-- concurrency/            thread pool、task scheduler、future/coroutine adapter
|   |-- support/                base、utils、config、error、memory、platform
|   |-- plugin/                 插件协议、插件管理、符号绑定
|   |-- eventbus/               自定义事件总线
|   |-- app/                    应用装配层、服务启动框架
|-- infra/
|   |-- logger/                 日志
|   |-- metrics/                指标
|   |-- tracing/                链路追踪预留
|-- protocol/
|   |-- transport/              tcp/udp/tls/kcp 抽象
|   |-- http/
|   |-- websocket/
|   |-- ftp/
|   |-- dns/
|   |-- bittorrent/
|-- libs/
|   |-- redis/
|   |-- rpc/
|-- server/
|   |-- http_server/
|   |-- match_server/
|   |-- gateway/
|-- plugins/
|-- examples/
|-- test/
```

说明：

- `core` 只负责“运行时和通用基础设施”
- `infra` 放基础横切能力，例如日志、指标、追踪
- `protocol` 只负责协议语义，不直接承担产品入口职责
- `server` 是真正可部署的业务进程
- `examples` 与 `test` 分开，避免样例和验证入口混杂
- `core/app` 负责承接未来上层 app 组合

## 4. 依赖方向

重构后依赖方向建议严格单向：

```text
support -> runtime -> app
support -> infra
runtime -> protocol
infra -> app / protocol / server
protocol -> server
libs -> core / infra
plugins -> plugin sdk / protocol extension points
test -> all
examples -> all
```

约束：

- `core` 不反向依赖具体协议
- `protocol` 不反向依赖 `server`
- `server` 负责装配，不下沉通用逻辑
- `plugins` 只依赖稳定 SDK，不直接侵入内部实现

## 5. 核心运行时重构

### 5.1 Runtime

`core/runtime` 聚焦下面几类对象：

- `EventLoop`
- `Poller`
- `Channel`
- `Connection`
- `Acceptor`
- `Connector`
- `Buffer`
- `Timer`

目标：

- 将现有网络运行时从“模块集合”提升为“清晰的统一运行时”
- 明确线程归属、事件分发、超时回调和连接生命周期
- 为协程 awaiter 预留挂接点

### 5.2 Concurrency

新增统一调度层，包含：

- `TaskScheduler`
- `Executor`
- `ThreadPool`
- `IoExecutor`
- `CoroutineAdapter`

作用：

- 把现在零散的线程池、后台任务、异步调用统一起来
- 形成 `同步 API / callback API / coroutine API` 三层兼容结构

### 5.3 EventBus

引入统一自定义事件模型：

- 进程内事件：模块间解耦
- 生命周期事件：服务启动、连接建立、协议注册
- 业务事件：上传完成、插件加载、路由变更

建议接口：

- `publish(event)`
- `subscribe<Event>(handler)`
- `unsubscribe(token)`

## 6. 协议层重构原则

### 6.1 协议层职责

协议层只做三件事：

- 协议编解码
- 协议状态机
- 协议会话抽象

不直接做：

- 业务入口装配
- 进程生命周期管理
- 全局配置装配
- 日志/指标/插件初始化

### 6.2 各协议演进建议

HTTP：

- 拆分 `server transport`、`request pipeline`、`content parser`、`route dispatch`
- 将上传下载、静态文件、代理能力从单体 `HttpServer` 中拆出为可组合组件

WebSocket：

- 复用 HTTP 握手层
- 将帧编解码、连接状态、业务 handler 分离

FTP：

- 继续沿命令分发模型演进
- 将控制连接与数据连接抽象成共享 runtime 组件

DNS：

- 保持轻量协议样例定位
- 后续可作为 UDP runtime 的回归验证模块

BitTorrent：

- 定位为实验协议模块
- 先清理边界，再决定是否提升为正式子系统

## 7. 插件与扩展模型

需求里明确提到：

- 支持插件扩展，包括协议、子模块等
- 支持自定义事件

因此插件体系建议升级为三层扩展点：

### 7.1 插件类型

- `ProtocolPlugin`
- `ServicePlugin`
- `ObserverPlugin`
- `CodecPlugin`

### 7.2 插件能力

- 注册协议 handler
- 注册路由或命令
- 订阅事件总线
- 注入配置项
- 注册 metrics / logger sink

### 7.3 稳定 SDK

必须提供最小稳定接口：

- 生命周期：`on_load / on_start / on_stop / on_unload`
- 上下文：`logger / config / event_bus / scheduler / registry`
- 能力注册：`register_protocol / register_service / subscribe_event`

## 8. app 层落点

需求中特别提到“后续的 app 需要结合 `core/app` 完成”。

因此 `core/app` 建议承担这些角色：

- 应用配置装配
- 服务生命周期管理
- 统一启动入口
- 资源依赖注入
- 运行模式切换（单线程 / 多线程 / 多进程）

建议抽象：

- `Application`
- `Service`
- `Bootstrap`
- `RuntimeContext`
- `ServiceRegistry`

服务进程只需描述：

- 需要哪些协议
- 需要哪些插件
- 需要哪些中间件
- 需要哪种运行模式

## 9. 进程与线程模型

需求要求“支持多进程和多线程模式可选”。

建议目标模型：

### 9.1 单进程单线程

- 适合调试
- 适合轻量服务

### 9.2 单进程多线程

- 1 个主 `EventLoop`
- N 个 worker loop / task executor
- 适合大多数协议服务

### 9.3 多进程模式

- 由 `app/bootstrap` 负责 master-worker 组织
- worker 进程运行独立 runtime
- 插件和配置在 worker 内装载

多进程能力第一阶段不一定立即实装，但文档和接口必须预留。

## 10. 构建系统重构

本轮已先从顶层 CMake 下手，方向如下：

- 用项目级 option 替代散落的构建开关
- 支持按模块开启/关闭：
  - test
  - plugins
  - libs
  - servers
  - example
- 为后续 `cmake/ProjectOptions.cmake`、`Warnings.cmake`、`Dependencies.cmake` 留扩展位

长期建议：

- 去掉顶层全局 `include_directories`
- 改为 target-based include/link
- 将第三方依赖封装成 interface target
- 将 install/export/package 能力补齐

## 11. 测试体系重构

当前 `test/` 偏样例式。

目标应拆成三层：

- `unit`：核心算法、parser、工具类
- `integration`：协议互通、事件循环、插件装配
- `examples`：演示型可执行文件

建议：

- `test/` 保留自动验证
- `examples/` 承接可手动运行样例
- `main.cpp` 退出“万能试验田”角色

## 12. 本轮设计结论

整体重构不建议采用“全仓一次性推翻”的方式，而应采用：

- 先统一目标架构
- 再抽离稳定运行时
- 再逐个协议迁移
- 最后收拢服务入口和插件系统

本轮完成的是：

- 明确项目级重构目标
- 明确目标分层和依赖方向
- 明确 `core/app` 的未来落点
- 明确插件、事件、异步、协程、多进程的架构位置
- 为后续分阶段迁移提供可执行蓝图
