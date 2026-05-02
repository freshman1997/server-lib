# Core 系统性重构任务拆分

日期：2026-05-02

## 目标

这轮重构的目标不是局部修补，而是把 `core` 的几个基础语义收紧：

1. 连接生命周期显式化：跨 await / timer / queue 的路径必须持有 owner 或 weak owner。
2. coroutine I/O completion 从 `ConnectionHandler` replacement 中拆出来。
3. `EventLoop` / `Poller` / `Channel` 的事件 token 闭环，避免 stale fd。
4. 关闭、timeout、异常、benchmark 和测试矩阵形成可持续的工程护栏。

## 总体路线

| 阶段 | 名称 | 目标 | 主要产物 | 风险 |
| --- | --- | --- | --- | --- |
| P0 | Baseline | 先冻结当前行为和性能基线 | 测试矩阵、benchmark、现有风险文档 | 低 |
| P1 | Connection Handle | 明确 owning / non-owning 连接语义 | `ConnectionHandle` / `ConnectionView` 或等价 API | 中 |
| P2 | Operation State | 让 awaiter/timer 生命周期安全 | shared operation state、weak timer callback | 中 |
| P3 | I/O Waiter | 移除 handler replacement | connection-local waiter registry | 高 |
| P4 | Event Token | 修正 fd reuse 防护 | Channel/PollEvent generation 闭环 | 中 |
| P5 | Close Semantics | 修正 graceful close 和 shutdown 行为 | close/write/shutdown 状态机测试 | 中 |
| P6 | API Cleanup | 收敛旧 API 和模块边界 | deprecated 删除、target 拆分计划 | 中 |

## P0：Baseline

状态：进行中

任务：

- [x] 扫描 core 风险并写入 [CORE_MODULE_REVIEW.md](/home/yuan/codes/test/webserver/docs/CORE_MODULE_REVIEW.md)。
- [x] 写测试矩阵和 benchmark 策略。
- [x] 增加轻量 benchmark target。
- [x] 跑当前 core 测试子集，记录通过/失败状态。
- [x] 跑 benchmark，记录重构前基线。
- [x] 写入 [CORE_BASELINE_RESULTS.md](/home/yuan/codes/test/webserver/docs/CORE_BASELINE_RESULTS.md)。

验收：

- `ctest` 中 core 现有用例状态可复现。
- benchmark 输出稳定，能作为后续阶段前后对比。

当前 baseline 注意事项：

- `async_facades` 已存在 segfault，位置在 one-shot timer 触发后再次 `cancel_timer(timer)`。
- 已用过渡方案修复：`WheelTimerManager` 现在持有 timer 对象到 manager 析构，避免已触发 one-shot timer 的 raw pointer 变成悬空指针。
- 后续仍应进入 P2 timer token / operation state，收敛 raw `Timer*` 句柄语义。

## P1：Connection Handle

目标：把 `ConnectionRef` 的混合语义拆开。

建议任务：

- [x] 新增 owning handle 类型。
  - 建议名：`ConnectionHandle`。
  - 内部持有 `std::shared_ptr<Connection>`。
  - 允许跨 await、timer、queue、callback。
- [x] 新增 non-owning view 类型。
  - 建议名：`ConnectionView`。
  - 内部只保存 `Connection*`。
  - 只用于当前调用栈，不允许保存到 awaiter 成员。
- [ ] 将 coroutine I/O awaiter 的成员从 `ConnectionRef` 改成 owning handle 或 shared operation state。
- [x] `RuntimeView::{read,write,flush,close,receive_from}` 的 `Connection*` 重载先保留，但内部优先 `shared_from_this()` 进入 owner 路径。
- [x] 为 `RuntimeView` 增加 `ConnectionHandle` I/O overload。
- [ ] 给 `Connection*` 重载加注释或 deprecated 标记，说明它只是迁移入口。
- [ ] 删除或降级 `ConnectionRef(Connection*)`。

验收：

- awaiter 不再跨挂起点保存裸 `Connection*`。
- 所有 async I/O API 的长期路径都能说明连接 owner 来自哪里。
- 原有协议层编译通过。

测试：

- async read 挂起后连接关闭。
- async read timeout 后连接再触发事件。
- async close 后外部 shared_ptr 释放。
- `register_connection(Connection*)` 对非 shared 管理对象应尽早失败或在文档中明确不支持。

当前进度：

- 已新增 `net/connection/connection_handle.h`。
- 已新增 `connection_handle` 回归测试，验证 handle 持有 owner、view 只观察当前连接。
- 已迁移 `test_async_facades` 的 immediate write/flush regression，用 `ConnectionHandle` 调用 `RuntimeView::write/flush`。
- 尚未迁移 awaiter，下一步应从 `RuntimeView` 的 shared_ptr 路径开始替换内部存储。

## P2：Operation State

目标：让 awaiter 和 timeout callback 不再互相持有悬空指针。

建议任务：

- [ ] 为每个 awaiter 引入 `OperationState`。
  - 保存 handle、result、completed、cancelled、connection weak/shared handle。
  - timer lambda 捕获 weak state。
- [ ] awaiter 析构只标记 cancelled，并取消 token/timer。
- [ ] timeout、close、error、read/write completion 都通过同一个 `complete_once()`。
- [ ] 统一 async read/write/flush/close/ssl handshake/receive 的状态转换。
- [x] 先修复 one-shot timer 触发后外部 cancel 的 UAF。
- [ ] 把 `Timer*` 作为外部长期句柄的语义收紧，后续替换为安全 token/handle。

验收：

- timer callback 不捕获 awaiter `this`。
- timeout 和事件同时发生只恢复一次 coroutine。
- awaiter 析构后残留 timer callback 不会访问已销毁对象。

测试：

- timeout 先发生，再触发 read。
- read 先发生，再触发 timeout tick。
- close/error 与 timeout 同轮 tick 竞争。
- coroutine frame 销毁后 timer 被触发。
- one-shot timer 触发后再次 cancel 不崩溃。
- 后续 API 明确禁止 raw pointer 长期持有，或通过安全 token 表达。

## P3：I/O Waiter Registry

目标：把 coroutine completion 从 `ConnectionHandler` replacement 中拆出来。

建议任务：

- [ ] 在 `Connection` 或具体 connection 中设计 waiter token。
  - `add_read_waiter`
  - `add_write_waiter`
  - `add_close_waiter`
  - `add_error_waiter`
  - `remove_waiter`
- [ ] `TcpConnection::on_read_event()` 读取完成后先更新状态，再唤醒 read waiters，再通知业务 handler。
- [ ] `TcpConnection::on_write_event()` flush/drain 后唤醒 write waiters。
- [ ] `do_close()` 唤醒 close/error waiters，不依赖业务 handler 是否存在。
- [ ] UDP receive awaiter 同样迁移到 waiter 模型。
- [ ] 删除 awaiter proxy handler 或只保留兼容层。

验收：

- async I/O 不调用 `set_connection_handler()`。
- 业务 handler 不会因为 coroutine await 被替换。
- 同一连接上的多个 observer / waiter 有定义良好的行为。

测试：

- 业务 handler + async read 同时存在。
- 两个 read waiter 同时注册时行为明确：拒绝第二个或按 FIFO 完成。
- write waiter 和 close waiter 同时存在。
- 协议层 handler 在 await 前后保持不变。

## P4：Event Token

目标：修正 `EventLoop` 与 `Poller` 的 generation/token 设计。

建议任务：

- [ ] 决定 generation 所属位置，推荐放在 `Channel`。
- [ ] `PollEvent` 总是携带非零 token/generation。
- [ ] `EpollPoller` 不维护一套独立但不同步的 generation。
- [ ] `EventLoop` 对所有事件强制校验 token。
- [ ] `close_channel/remove_channel` 后让旧 token 失效。
- [ ] select/poll/kqueue 后端也保持同一语义。

验收：

- fd reuse 不会把旧事件投递到新 channel。
- generation 字段不再有“0 表示跳过校验”的常态路径。

测试：

- 人工构造 stale `PollEvent`。
- close channel 后重复 close/remove。
- 快速创建/关闭连接并复用 fd。

## P5：Close Semantics

目标：修正 TCP/UDP 关闭状态机。

建议任务：

- [ ] `TcpConnection::close()` 在 pending output 下进入 draining 状态。
- [ ] `on_write_event()` 在 closing/draining 状态先 flush，output 为空后再 `do_close()`。
- [ ] 明确 `shutdown_write()`、peer input shutdown、local close 的组合行为。
- [ ] `get_local_address()` 返回真实 local address。
- [ ] UDP idle close / abort / graceful close 状态命名统一。

验收：

- `write + close` 不丢尾包。
- input shutdown 时仍可写完响应。
- local/remote address 日志正确。

测试：

- write large payload then close。
- peer half-close 后 server 写响应。
- close while connecting。
- close twice / abort then close。

## P6：API Cleanup 与模块边界

目标：把临时兼容层收掉，降低后续协议层成本。

建议任务：

- [ ] 删除 `ConnectionRef` 或只保留同步 view。
- [ ] 删除 `Task<T>::operator T()`。
- [ ] `Task<T>::execute()` 改名或实现真正 sync wait。
- [ ] detached task 增加异常 sink。
- [ ] `net/secuity` 改成 `net/security`。
- [ ] 评估 CMake target 拆分：
  - `CoreBuffer`
  - `CoreTimer`
  - `CoreEvent`
  - `CoreNet`
  - `CoreCoroutine`

验收：

- 新 API 名字能表达生命周期和行为。
- core 内部依赖方向清晰。
- 协议层不依赖临时兼容类型。

## 推进规则

- 每个阶段先跑 `test/core` 和 benchmark baseline。
- 每个阶段至少补 2 个针对本阶段风险的测试。
- 对协议层有破坏性时，先保留兼容 wrapper，再迁移调用点。
- 重构阶段不要同时做大面积格式化，方便 review。
- 每完成一阶段，更新测试矩阵中的状态。
