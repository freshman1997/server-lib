# Core 测试矩阵与 Benchmark 计划

日期：2026-05-02

## 测试分层

| 层级 | 目标 | 位置 | 是否进 CTest | 运行频率 |
| --- | --- | --- | --- | --- |
| Unit | 单个类型/状态机行为 | `test/core` | 是 | 每次改动 |
| Integration | EventLoop + Connection + Coroutine 组合 | `test/core` / `test/network` | 是 | 每个阶段 |
| Regression | 曾发现的 bug 场景 | `test/core` | 是 | 每次改动 |
| Stress | 大量连接、重复 timeout、fd reuse | `test/network` 或单独工具 | 可选 | 阶段结束 |
| Benchmark | 吞吐、延迟、分配趋势 | `test/benchmark` | 否 | 阶段前后 |

## 核心测试矩阵

| 编号 | 模块 | 场景 | 当前覆盖 | 目标测试 | 优先级 |
| --- | --- | --- | --- | --- | --- |
| C01 | ConnectionHandle | shared owner 跨 await 后仍有效 | 已补 `connection_handle` | 后续补 await 跨挂起版本 | P1 |
| C02 | ConnectionView | view 不可跨 await 保存 | 无 | 编译/API 约束或文档测试 | P1 |
| C03 | RuntimeView | `Connection*` async API 内部转 shared owner | 部分 | 增加 shared_from_this 路径测试 | P1 |
| C04 | AsyncRead | 挂起后 close 恢复 closed | 部分 | `async_read_close_resume` | P1/P2 |
| C05 | AsyncRead | timeout 后 read 事件不再恢复第二次 | 无 | `async_read_timeout_then_event` | P2 |
| C06 | AsyncRead | read 事件和 timeout 同轮竞争 | 无 | fake timer 或短 timeout race | P2 |
| C07 | AsyncWrite | 小包立即写完不挂起 | 部分 | loopback immediate completion | P2/P3 |
| C08 | AsyncWrite | 写入 pending 后 close 不丢尾包 | 无 | `write_then_close_drains` | P5 |
| C09 | AsyncFlush | output drain 后恢复 | 部分 | `async_flush_drain` | P3 |
| C10 | AsyncClose | close 已完成时立即返回 | 部分 | closed state fast path | P2 |
| C11 | SSL Handshake | timeout 后 callback 不访问 awaiter | 无 | fake SSL handler | P2 |
| C12 | Datagram | receive timeout 后 packet 到达不二次恢复 | 无 | UDP fake instance | P2/P3 |
| C13 | Handler | 业务 handler 不被 awaiter 替换 | 无 | await 前后 owner 不变 | P3 |
| C14 | Handler | 两个 read waiter 同时等待行为明确 | 无 | reject/FIFO 测试 | P3 |
| C15 | EventLoop | `post_coroutine` 不导致长期 loop 非预期退出 | 部分 | run mode 分离后新增 | P4 |
| C16 | EventLoop | queue callback 异常被捕获并继续运行 | 部分 | callback throw regression | P4 |
| C17 | Poller | stale fd event 被丢弃 | 无 | fake poller token mismatch | P4 |
| C18 | Poller | close/remove/update 重复调用安全 | 无 | fake channel lifecycle | P4 |
| C19 | TcpConnection | `get_local_address()` 返回真实 local | 无 | loopback getsockname | P5 |
| C20 | TcpConnection | peer half-close 后仍可写响应 | 无 | socketpair/loopback | P5 |
| C21 | TcpConnection | close twice / abort then close 幂等 | 部分 | lifecycle regression | P5 |
| C22 | Task | `execute()` 不伪装 run-to-completion | 无 | API cleanup 后测试 | P6 |
| C23 | Task | detached exception 有 sink | 无 | logger/error hook 测试 | P6 |
| C24 | Timer | cancel 后 callback 不触达已销毁 state | 无 | weak operation state 测试 | P2 |
| C25 | Buffer | append/copy/consume/compact 基础行为 | 有 | 保持现有 `buffer_model` | P0 |
| C26 | Timer | one-shot timer 触发释放后外部 cancel 不崩溃 | 已补 `timer_lifecycle` | 安全 timer token 或明确 API 约束 | P2 |

## 阶段验收测试集

### P0 Baseline

必须跑：

```bash
cd build
ctest -R "coroutine_runtime|connection_handle|event_bus|buffer_model|byte_buffer_reader|time_api|timer_lifecycle|async_facades|ipv6_dual_stack|base64" --output-on-failure
cd ..
./build/test/benchmark/core_runtime_benchmark
```

### P1 Connection Handle

必须新增/运行：

- `connection_handle_lifetime`
- `runtime_view_connection_pointer_compat`
- `async_read_close_resume`

### P2 Operation State

必须新增/运行：

- `async_read_timeout_then_event`
- `async_write_timeout_then_close`
- `ssl_handshake_timeout_cleanup`
- `timer_cancel_after_awaiter_destroy`
- `oneshot_timer_cancel_after_fire`

### P3 I/O Waiter

必须新增/运行：

- `handler_not_replaced_by_async_read`
- `multiple_waiters_policy`
- `write_waiter_and_close_waiter`
- `datagram_receive_waiter`

### P4 Event Token

必须新增/运行：

- `poller_stale_generation_ignored`
- `close_channel_generation_increment`
- `fd_reuse_old_event_ignored`

### P5 Close Semantics

必须新增/运行：

- `tcp_write_then_close_drains`
- `tcp_half_close_write_response`
- `tcp_local_address`
- `tcp_abort_close_idempotent`

### P6 API Cleanup

必须新增/运行：

- `task_resume_once_semantics`
- `detached_task_exception_sink`
- 协议层 smoke：HTTP、MQTT、Socks5、SSH 可按当前启用情况运行。

## Benchmark 指标

当前先用轻量 benchmark，不引入 Google Benchmark，避免依赖成本。输出字段固定，方便脚本采集。

| 编号 | 名称 | 指标 | 用途 |
| --- | --- | --- | --- |
| B01 | `byte_buffer_append_copy` | ops/s、MiB/s | 监控 buffer 基础吞吐 |
| B02 | `buffer_chain_push_pop` | ops/s、MiB/s | 监控输出队列和 chunk 操作 |
| B03 | `event_bus_publish` | ops/s | 监控 EventBus 分发成本 |
| B04 | `event_loop_callback_roundtrip` | ops/s、平均延迟 | P4 后加入，监控 queue/post 路径 |
| B05 | `tcp_loopback_echo` | req/s、p50/p95/p99 | P3/P5 后加入，监控连接 I/O |
| B06 | `async_read_write_roundtrip` | op/s、p95 | P3 后加入，监控 waiter 模型 |
| B07 | `timer_schedule_cancel` | ops/s | P2 后加入，监控 OperationState/timer 成本 |

## Benchmark 运行方法

构建：

```bash
cmake --build build --target core_runtime_benchmark
```

运行：

```bash
./build/test/benchmark/core_runtime_benchmark
```

建议每次阶段重构前后各跑 3 次，记录：

- git commit / branch
- build type
- CPU 型号
- 是否开启 ASAN/TSAN
- benchmark 输出

## 性能阈值建议

短期先做趋势观察，不用硬卡 CI。等 baseline 稳定后再设置阈值：

- buffer/eventbus 类微基准：单阶段下降超过 10% 需要解释。
- event loop callback：单阶段下降超过 15% 需要解释。
- TCP loopback：p95 延迟上升超过 20% 需要解释。
- 如果重构为了安全性引入小幅成本，文档里记录取舍即可。
