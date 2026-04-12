# 项目整体重构演进方式

## 原则

演进方式采用“分阶段、可回滚、不中断现有协议能力”的策略。

编译与平台原则：

- 优先保证 MinGW 可编译
- 同步兼顾 Linux 和 macOS
- 不把 MSVC 兼容问题作为本轮重构主线

不能做的事：

- 一次性重写全部模块
- 在没有兼容层的情况下直接替换运行时
- 同时迁移 HTTP / FTP / WebSocket / 插件 / app 层

应该做的事：

- 先抽公共骨架
- 再迁移一条主链路
- 每阶段都保留可编译、可验证状态

## 阶段 0：收口工程入口

目标：

- 统一顶层 CMake 开关
- 统一模块装配策略
- 为模块拆分建立边界

本轮已完成：

- 顶层工程开关统一
- 文档化目标架构与演进路线

产物：

- `refctor.md`
- `docs/PROJECT_REFACTOR_ARCHITECTURE.md`
- `docs/PROJECT_REFACTOR_ROADMAP.md`
- 顶层 `CMakeLists.txt`

## 阶段 1：抽离核心运行时

目标：

- 固化 `EventLoop / Poller / Connection / Buffer / Timer`
- 整理线程模型与任务调度接口
- 为协程 awaitable 接口预留适配点

动作：

- 清理 `core` 内部职责边界
- 将 runtime、concurrency、support 拆清楚
- 补齐运行时回归测试

完成标志：

- `core` 不依赖具体协议
- runtime 具备稳定 API
- 最少有 1 组单元测试和 1 组集成测试覆盖

## 阶段 2：建设 app 装配层

目标：

- 让 `core/app` 成为统一服务启动框架
- 明确 `Application / Service / Bootstrap / RuntimeContext`

动作：

- 把现在散落在 `main.cpp`、测试入口、server 里的初始化逻辑收口
- 建立统一配置装配与生命周期管理

完成标志：

- 业务服务不再直接拼装底层 runtime
- `core/app` 可独立启动一个最小服务

## 阶段 3：迁移 HTTP 主链路

优先选 HTTP，因为它最成熟、覆盖能力最多。

目标：

- 拆分 `HttpServer`
- 把路由、解析、上传下载、代理、中间件分成可组合组件

动作：

- 拆 `transport`
- 拆 `request pipeline`
- 拆 `dispatcher`
- 拆 `feature module`

完成标志：

- HTTP 可以通过 app 层启动
- 上传、下载、静态文件、代理能力不再全堆在一个大对象里

## 阶段 4：迁移 WebSocket / FTP

目标：

- WebSocket 复用 HTTP 握手层与 runtime
- FTP 对齐统一连接与会话模型

动作：

- 将 WebSocket 的连接状态机独立出来
- 将 FTP 控制连接/数据连接建立成可复用会话框架

完成标志：

- WebSocket 和 FTP 都能通过统一 app 层启动
- 各协议不再重复拼 runtime 能力

## 阶段 5：统一事件与插件系统

目标：

- 引入统一 EventBus
- 让插件通过稳定 SDK 扩展协议和服务

动作：

- 给插件增加生命周期与上下文
- 支持注册事件监听、协议 handler、服务扩展

完成标志：

- 插件不再直接侵入内部实现
- 协议扩展具备稳定扩展点

## 阶段 6：协程化与异步 API

目标：

- 在保持现有同步/回调接口的前提下，引入协程友好的 API

动作：

- 为连接、定时器、任务调度提供 awaitable wrapper
- 给 HTTP client / Redis / RPC 预留 coroutine 接口

完成标志：

- 至少一条完整业务链支持 coroutine API
- 老接口仍然可用

## 阶段 7：多进程化与部署模型

目标：

- 支持 master-worker 或独立 server 进程模式

动作：

- 在 `server/` 层定义进程模型
- 明确配置下发、worker 初始化、插件加载方式

完成标志：

- 具备单进程/多线程和多进程两种运行模式

## 推荐迁移顺序

建议按下面顺序推进，而不是并行大爆炸：

1. 顶层构建与文档
2. `core/runtime`
3. `core/app`
4. `protocol/http`
5. `protocol/websocket`
6. `protocol/ftp`
7. `plugins`
8. `libs/redis_cli`
9. `libs/rpc`
10. `server/*`

## 风险控制

每个阶段都建议遵守下面的约束：

- 旧入口先保留，新增入口先并行
- 不做跨 3 个以上模块的大批量同时迁移
- 每迁移一个模块，就补对应测试
- 文档先行，接口冻结后再迁移实现

## 最终目标形态

最终项目应形成：

- 一个稳定的底层 runtime
- 一个统一的 app 装配层
- 多个可独立演进的协议模块
- 一套稳定插件 SDK
- 一组可部署的 server 产物
- 一组真正自动化的测试体系
