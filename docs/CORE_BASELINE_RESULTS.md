# Core Baseline Results

日期：2026-05-02

## 构建

命令：

```bash
cmake -S . -B build
cmake --build build --target core_runtime_benchmark -j 4
```

结果：

- CMake configure 成功。
- `core_runtime_benchmark` 构建成功。

## Benchmark

命令：

```bash
./build/test/benchmark/core_runtime_benchmark
```

结果：

```text
core runtime benchmark
build=manual chrono=steady_clock
byte_buffer_append_copy        ops=200000       elapsed_ms=70.449     ops_per_s=2.83893e+06    MiB_per_s=2772.4
buffer_chain_push_pop          ops=800000       elapsed_ms=737.076    ops_per_s=1.08537e+06    MiB_per_s=529.966
event_bus_publish              ops=300000       elapsed_ms=237.356    ops_per_s=1.26392e+06
```

说明：

- 当前 benchmark 是 Debug build 下的粗基线。
- 后续性能比较应尽量使用同一 build type 和同一机器。

## Core CTest 子集

命令：

```bash
cd build
ctest -R "coroutine_runtime|event_bus|buffer_model|byte_buffer_reader|async_facades|ipv6_dual_stack|base64" --output-on-failure
```

结果：

| 测试 | 结果 |
| --- | --- |
| `coroutine_runtime` | Passed |
| `event_bus` | Passed |
| `buffer_model` | Passed |
| `byte_buffer_reader` | Passed |
| `async_facades` | SegFault |
| `ipv6_dual_stack` | Passed |
| `base64` | Passed |

`async_facades` 当前崩溃点：

```text
Program received signal SIGSEGV, Segmentation fault.
#0  0x000055555568aa60 in ?? ()
#1  yuan::net::NetworkRuntime::cancel_timer(timer=0x555555670c10)
    at core/core/src/net/runtime/network_runtime.cpp:130
#2  test_network_runtime_lifecycle()
    at test/core/test_async_facades.cpp:397
```

初步判断：

- `test_network_runtime_lifecycle()` 中 `schedule(10, ...)` 创建的是 one-shot timer。
- `sync_wait()` 等待期间 timer 已触发并可能被 timer manager 删除。
- 测试随后调用 `runtime.cancel_timer(timer)`，对已释放 timer 指针调用 `cancel()`，触发 UAF/segfault。

这和本轮重构里的 P2 `OperationState` / timer token 问题相关，应作为优先回归用例处理。

## P2 第一刀修复后

变更：

- `WheelTimerManager` 改为持有创建出来的 `WheelTimer` 到 manager 析构。
- `tick()` 对完成的 one-shot timer 不再立即 `delete`。
- 新增 `timer_lifecycle` 回归测试，覆盖 one-shot timer 触发后再次 cancel。

命令：

```bash
cd build
ctest -R "coroutine_runtime|event_bus|buffer_model|byte_buffer_reader|time_api|timer_lifecycle|async_facades|ipv6_dual_stack|base64" --output-on-failure
```

结果：

```text
100% tests passed, 0 tests failed out of 9
```

Benchmark：

```text
byte_buffer_append_copy        ops=200000       elapsed_ms=65.5771    ops_per_s=3.04984e+06    MiB_per_s=2978.36
buffer_chain_push_pop          ops=800000       elapsed_ms=732.793    ops_per_s=1.09171e+06    MiB_per_s=533.063
event_bus_publish              ops=300000       elapsed_ms=236.397    ops_per_s=1.26905e+06
```

## P1 第一刀后

变更：

- 新增 `ConnectionHandle`：持有 `std::shared_ptr<Connection>`，用于后续跨 await / timer / queue 的安全连接句柄。
- 新增 `ConnectionView`：只保存 `Connection*`，用于当前调用栈内的非 owning 观察。
- 新增 `connection_handle` 测试。

命令：

```bash
cd build
ctest -R "coroutine_runtime|connection_handle|event_bus|buffer_model|byte_buffer_reader|time_api|timer_lifecycle|async_facades|ipv6_dual_stack|base64" --output-on-failure
```

结果：

```text
100% tests passed, 0 tests failed out of 10
```

## P1 第二刀后

变更：

- `RuntimeView` 增加 `ConnectionHandle` overload：
  - `read`
  - `write`
  - `flush`
  - `close`
  - `ssl_handshake`
  - `receive_from`
- `RuntimeView` 的 `Connection*` overload 会优先尝试 `shared_from_this()`，让 shared 管理的裸指针调用自动进入 owner 路径。
- `test_async_facades` 的 immediate write/flush regression 已迁移到 `ConnectionHandle` 调用。

命令：

```bash
cd build
ctest -R "coroutine_runtime|connection_handle|event_bus|buffer_model|byte_buffer_reader|time_api|timer_lifecycle|async_facades|ipv6_dual_stack|base64" --output-on-failure
```

结果：

```text
100% tests passed, 0 tests failed out of 10
```

Benchmark：

```text
byte_buffer_append_copy        ops=200000       elapsed_ms=67.3703    ops_per_s=2.96867e+06    MiB_per_s=2899.09
buffer_chain_push_pop          ops=800000       elapsed_ms=730.913    ops_per_s=1.09452e+06    MiB_per_s=534.434
event_bus_publish              ops=300000       elapsed_ms=236.273    ops_per_s=1.26972e+06
```

## P1 第三刀后

变更：

- `stream_io_awaitable`、`datagram_io_awaitable`、`connection_event_awaitable` 的跨挂起连接成员从 `ConnectionRef` 改为 `ConnectionHandle`。
- 删除 coroutine I/O API 的裸 `Connection*` 重载，`RuntimeView` 和 `NetworkRuntime::RuntimeView` 不再允许裸指针进入 async read/write/flush/close/ssl/receive 路径。
- 迁移协议层少量直接调用点到 `std::shared_ptr<Connection>` 路径。
- 新增 `async_facades` 回归，覆盖 read 挂起后本地 shared_ptr 释放，`ConnectionHandle` 仍持有连接直到 delayed close 恢复。

命令：

```bash
cd build
ctest -R "coroutine_runtime|connection_handle|event_bus|buffer_model|byte_buffer_reader|time_api|timer_lifecycle|async_facades|ipv6_dual_stack|base64" --output-on-failure
```

结果：

```text
100% tests passed, 0 tests failed out of 10
```

Benchmark：

```text
byte_buffer_append_copy        ops=200000       elapsed_ms=9.77272    ops_per_s=2.04651e+07    MiB_per_s=19985.5
buffer_chain_push_pop          ops=800000       elapsed_ms=30.2345    ops_per_s=2.64598e+07    MiB_per_s=12919.8
event_bus_publish              ops=300000       elapsed_ms=10.4021    ops_per_s=2.88402e+07
```

## P2 第一刀后

变更：

- 新增 `coroutine/awaiter_timeout_state.h`，统一 coroutine timeout state。
- stream read/write/flush、SSL handshake、datagram receive、connect timeout 的 timer lambda 不再捕获 awaiter `this`，改为捕获 weak state。
- awaiter resume/destructor 路径会标记 timeout state cancelled/completed 并取消 timer。
- 新增 `async_facades` 回归：`async_read` timeout 后 late input 到达不会二次恢复，数据仍留在连接 buffer。

命令：

```bash
cd build
ctest -R "coroutine_runtime|connection_handle|event_bus|buffer_model|byte_buffer_reader|time_api|timer_lifecycle|async_facades|ipv6_dual_stack|base64" --output-on-failure
```

结果：

```text
100% tests passed, 0 tests failed out of 10
```

Benchmark：

```text
byte_buffer_append_copy        ops=200000       elapsed_ms=10.5519    ops_per_s=1.89539e+07    MiB_per_s=18509.7
buffer_chain_push_pop          ops=800000       elapsed_ms=31.2107    ops_per_s=2.56322e+07    MiB_per_s=12515.7
event_bus_publish              ops=300000       elapsed_ms=10.6967    ops_per_s=2.80462e+07
```

## P3 第一刀后

变更：

- 删除 stream read/write/flush/close、datagram receive、connection event awaiter 中遗留的 proxy handler 类、proxy owner、restore 兼容逻辑。
- 这些 awaiter 现在只依赖 `Connection::add_event_waiter/remove_event_waiter`，不再替换业务 `ConnectionHandler`。
- 新增 `async_facades` 回归，验证 read/flush/close awaiter 不替换已安装 handler。

保留风险：

- `connect_awaitable` 和 `accept_awaitable` 仍使用 handler replacement，需要后续为 connector/acceptor 设计独立 waiter registry。

命令：

```bash
cd build
ctest -R "coroutine_runtime|connection_handle|event_bus|buffer_model|byte_buffer_reader|time_api|timer_lifecycle|async_facades|ipv6_dual_stack|base64" --output-on-failure
```

结果：

```text
100% tests passed, 0 tests failed out of 10
```

Benchmark：

```text
byte_buffer_append_copy        ops=200000       elapsed_ms=12.8399    ops_per_s=1.55764e+07    MiB_per_s=15211.4
buffer_chain_push_pop          ops=800000       elapsed_ms=33.0233    ops_per_s=2.42253e+07    MiB_per_s=11828.8
event_bus_publish              ops=300000       elapsed_ms=13.5062    ops_per_s=2.2212e+07
```

## P4 第一刀后

变更：

- `Channel` 增加 generation，并成为唯一 generation 来源。
- epoll/select/poll/kqueue poller 都从 `Channel::generation()` 填充 `PollEvent::generation`。
- `EventLoop` 删除独立 `channel_generations_`，对所有事件强制校验 generation，`generation == 0` 不再绕过校验。
- `close_channel()` 会 bump generation，使旧事件 token 失效。
- 新增 `event_token` 回归测试。

命令：

```bash
cd build
ctest -R "coroutine_runtime|connection_handle|event_bus|buffer_model|byte_buffer_reader|time_api|timer_lifecycle|async_facades|ipv6_dual_stack|base64|event_token" --output-on-failure
```

结果：

```text
100% tests passed, 0 tests failed out of 11
```

Benchmark：

```text
byte_buffer_append_copy        ops=200000       elapsed_ms=9.65074    ops_per_s=2.07238e+07    MiB_per_s=20238.1
buffer_chain_push_pop          ops=800000       elapsed_ms=34.8065    ops_per_s=2.29842e+07    MiB_per_s=11222.8
event_bus_publish              ops=300000       elapsed_ms=11.6267    ops_per_s=2.58026e+07
```

## P5 第一刀后

变更：

- `TcpConnection::get_local_address()` 返回真实 local address，不再返回 remote address。
- 连接 init 和 connect complete 后刷新 local address。
- `write_and_flush` / `write_owned_and_flush` 允许 closing 状态继续 flush pending output。
- `on_write_event()` 在 closing 状态先 flush，output drain 后再通知 writable/on_write 并 `do_close()`。
- 新增 `tcp_close_semantics` 回归测试，覆盖 local/remote port 区分、write/flush/read/close 基础链路。

命令：

```bash
cd build
ctest -R "coroutine_runtime|connection_handle|event_bus|buffer_model|byte_buffer_reader|time_api|timer_lifecycle|async_facades|ipv6_dual_stack|base64|event_token|tcp_close_semantics" --output-on-failure
```

结果：

```text
100% tests passed, 0 tests failed out of 12
```

Benchmark：

```text
byte_buffer_append_copy        ops=200000       elapsed_ms=15.4986    ops_per_s=1.29044e+07    MiB_per_s=12601.9
buffer_chain_push_pop          ops=800000       elapsed_ms=36.3181    ops_per_s=2.20276e+07    MiB_per_s=10755.6
event_bus_publish              ops=300000       elapsed_ms=13.6602    ops_per_s=2.19617e+07
```

结果：

```text
100% tests passed, 0 tests failed out of 12
```

Benchmark：

```text
byte_buffer_append_copy        ops=200000       elapsed_ms=10.4415    ops_per_s=1.91544e+07    MiB_per_s=18705.5
buffer_chain_push_pop          ops=800000       elapsed_ms=32.7844    ops_per_s=2.44018e+07    MiB_per_s=11915
event_bus_publish              ops=300000       elapsed_ms=11.6819    ops_per_s=2.56808e+07
```

结果：

```text
100% tests passed, 0 tests failed out of 12
```

Benchmark：

```text
byte_buffer_append_copy        ops=200000       elapsed_ms=10.4415    ops_per_s=1.91544e+07    MiB_per_s=18705.5
buffer_chain_push_pop          ops=800000       elapsed_ms=32.7844    ops_per_s=2.44018e+07    MiB_per_s=11915
event_bus_publish              ops=300000       elapsed_ms=11.6819    ops_per_s=2.56808e+07
```

结果：

```text
100% tests passed, 0 tests failed out of 12
```

Benchmark：

```text
byte_buffer_append_copy        ops=200000       elapsed_ms=10.4415    ops_per_s=1.91544e+07    MiB_per_s=18705.5
buffer_chain_push_pop          ops=800000       elapsed_ms=32.7844    ops_per_s=2.44018e+07    MiB_per_s=11915
event_bus_publish              ops=300000       elapsed_ms=11.6819    ops_per_s=2.56808e+07
```

结果：

```text
100% tests passed, 0 tests failed out of 12
```

Benchmark：

```text
byte_buffer_append_copy        ops=200000       elapsed_ms=10.6643    ops_per_s=1.87542e+07    MiB_per_s=18314.7
buffer_chain_push_pop          ops=800000       elapsed_ms=31.49      ops_per_s=2.54049e+07    MiB_per_s=12404.7
event_bus_publish              ops=300000       elapsed_ms=10.8732    ops_per_s=2.75909e+07
```

结果：

```text
100% tests passed, 0 tests failed out of 12
```

Benchmark：

```text
byte_buffer_append_copy        ops=200000       elapsed_ms=13.276     ops_per_s=1.50647e+07    MiB_per_s=14711.7
buffer_chain_push_pop          ops=800000       elapsed_ms=33.6153    ops_per_s=2.37987e+07    MiB_per_s=11620.4
event_bus_publish              ops=300000       elapsed_ms=11.4749    ops_per_s=2.6144e+07
```

结果：

```text
100% tests passed, 0 tests failed out of 12
```

Benchmark：

```text
byte_buffer_append_copy        ops=200000       elapsed_ms=13.276     ops_per_s=1.50647e+07    MiB_per_s=14711.7
buffer_chain_push_pop          ops=800000       elapsed_ms=33.6153    ops_per_s=2.37987e+07    MiB_per_s=11620.4
event_bus_publish              ops=300000       elapsed_ms=11.4749    ops_per_s=2.6144e+07
```

结果：

```text
100% tests passed, 0 tests failed out of 12
```

Benchmark：

```text
byte_buffer_append_copy        ops=200000       elapsed_ms=10.394     ops_per_s=1.92419e+07    MiB_per_s=18791
buffer_chain_push_pop          ops=800000       elapsed_ms=32.2529    ops_per_s=2.4804e+07     MiB_per_s=12111.3
event_bus_publish              ops=300000       elapsed_ms=10.86      ops_per_s=2.76244e+07
```

Benchmark：

```text
byte_buffer_append_copy        ops=200000       elapsed_ms=13.0076    ops_per_s=1.53757e+07    MiB_per_s=15015.3
buffer_chain_push_pop          ops=800000       elapsed_ms=38.978     ops_per_s=2.05244e+07    MiB_per_s=10021.7
event_bus_publish              ops=300000       elapsed_ms=12.5144    ops_per_s=2.39724e+07
```

## P6 第一刀后

变更：

- 删除 `Task<T>::operator T()`，避免 task 未完成时隐式读取默认值。
- `Task<T>::execute()` / `Task<void>::execute()` 改名为 `resume_once_and_get_result()`，明确它只 resume 一次并读取当前结果。
- `sync_wait()` 无 event loop fallback 改用新 API。
- `coroutine_runtime` 增加 task API 回归，覆盖 immediate result 和异常 rethrow。

命令：

```bash
cd build
ctest -R "coroutine_runtime|connection_handle|event_bus|buffer_model|byte_buffer_reader|time_api|timer_lifecycle|async_facades|ipv6_dual_stack|base64|event_token|tcp_close_semantics" --output-on-failure
```

结果：

```text
100% tests passed, 0 tests failed out of 12
```

Benchmark：

```text
byte_buffer_append_copy        ops=200000       elapsed_ms=9.63373    ops_per_s=2.07604e+07    MiB_per_s=20273.8
buffer_chain_push_pop          ops=800000       elapsed_ms=33.4907    ops_per_s=2.38872e+07    MiB_per_s=11663.7
event_bus_publish              ops=300000       elapsed_ms=12.2284    ops_per_s=2.45331e+07
```

## P6 第二刀后

变更：

- 删除 `net/connection/connection_ref.h`。
- `connection_handle` 测试不再覆盖 `ConnectionRef` 的 raw pointer promotion，后续只保留 `ConnectionHandle` / `ConnectionView` 两种清晰语义。

命令：

```bash
cd build
ctest -R "coroutine_runtime|connection_handle|event_bus|buffer_model|byte_buffer_reader|time_api|timer_lifecycle|async_facades|ipv6_dual_stack|base64|event_token|tcp_close_semantics" --output-on-failure
```

结果：

```text
100% tests passed, 0 tests failed out of 12
```

## P6 第三刀后

变更：

- `Task<void>` 增加 detached exception handler。
- detached coroutine 在 final suspend 或已完成后 detach 时，会把未处理异常投递到 handler。
- `coroutine_runtime` 增加 detached exception sink 回归，验证 sink 能收到原始异常。

命令：

```bash
cd build
ctest -R "coroutine_runtime|connection_handle|event_bus|buffer_model|byte_buffer_reader|time_api|timer_lifecycle|async_facades|ipv6_dual_stack|base64|event_token|tcp_close_semantics" --output-on-failure
```

结果：

```text
100% tests passed, 0 tests failed out of 12
```

Benchmark：

```text
byte_buffer_append_copy        ops=200000       elapsed_ms=13.276     ops_per_s=1.50647e+07    MiB_per_s=14711.7
buffer_chain_push_pop          ops=800000       elapsed_ms=33.6153    ops_per_s=2.37987e+07    MiB_per_s=11620.4
event_bus_publish              ops=300000       elapsed_ms=11.4749    ops_per_s=2.6144e+07
```

## P5 第二刀后

变更：

- `AsyncConnectionContext` / `AsyncListenerHost` 默认 handler 不再在 peer input shutdown 时主动 close。
- `AsyncClientSession` 补充 read overload，可显式控制 terminal event 是否用已缓冲数据完成。
- `tcp_close_semantics` 扩展覆盖 large payload write+close drain、peer half-close 后写响应、close/abort 幂等。

命令：

```bash
cd build
ctest -R "coroutine_runtime|connection_handle|event_bus|buffer_model|byte_buffer_reader|time_api|timer_lifecycle|async_facades|ipv6_dual_stack|base64|event_token|tcp_close_semantics" --output-on-failure
```

结果：

```text
100% tests passed, 0 tests failed out of 12
```

Benchmark：

```text
byte_buffer_append_copy        ops=200000       elapsed_ms=10.6643    ops_per_s=1.87542e+07    MiB_per_s=18314.7
buffer_chain_push_pop          ops=800000       elapsed_ms=31.49      ops_per_s=2.54049e+07    MiB_per_s=12404.7
event_bus_publish              ops=300000       elapsed_ms=10.8732    ops_per_s=2.75909e+07
```

## P3 第二刀后

变更：

- `StreamAcceptor` 增加 accept waiter registry，`TcpAcceptor` 接受连接后通知 waiters。
- `accept_awaitable` 改为注册 accept waiter，不再替换 acceptor handler。
- `connect_awaitable` 改为注册 connection event waiter，不再安装 proxy connection handler。
- `TcpConnection` connecting 完成/失败路径即使没有业务 handler 也会触发 connected/error waiters。
- `async_facades` 增加 connect/accept awaiter 不替换 handler 回归。

命令：

```bash
cd build
ctest -R "coroutine_runtime|connection_handle|event_bus|buffer_model|byte_buffer_reader|time_api|timer_lifecycle|async_facades|ipv6_dual_stack|base64|event_token|tcp_close_semantics" --output-on-failure
```

结果：

```text
100% tests passed, 0 tests failed out of 12
```

Benchmark：

```text
byte_buffer_append_copy        ops=200000       elapsed_ms=10.4415    ops_per_s=1.91544e+07    MiB_per_s=18705.5
buffer_chain_push_pop          ops=800000       elapsed_ms=32.7844    ops_per_s=2.44018e+07    MiB_per_s=11915
event_bus_publish              ops=300000       elapsed_ms=11.6819    ops_per_s=2.56808e+07
```

## P3 第三刀后

变更：

- `Connection` 增加 `has_event_waiter()` 查询，用于明确 waiter 冲突策略。
- `async_read` 在同一连接已有 readable waiter 时返回 `invalid_state`，拒绝第二个 concurrent read waiter。
- `async_facades` 增加 multiple read waiter policy 回归，验证第二个 read 被拒绝且第一个 waiter 仍正常完成。

命令：

```bash
cd build
ctest -R "coroutine_runtime|connection_handle|event_bus|buffer_model|byte_buffer_reader|time_api|timer_lifecycle|async_facades|ipv6_dual_stack|base64|event_token|tcp_close_semantics" --output-on-failure
```

结果：

```text
100% tests passed, 0 tests failed out of 12
```

Benchmark：

```text
byte_buffer_append_copy        ops=200000       elapsed_ms=15.4986    ops_per_s=1.29044e+07    MiB_per_s=12601.9
buffer_chain_push_pop          ops=800000       elapsed_ms=36.3181    ops_per_s=2.20276e+07    MiB_per_s=10755.6
event_bus_publish              ops=300000       elapsed_ms=13.6602    ops_per_s=2.19617e+07
```

## P5 第三刀后

变更：

- `TcpConnection::close()` 在 connecting 状态直接进入 close 流程并恢复 close waiter。
- `tcp_close_semantics` 增加 close while connecting 回归，覆盖新建 connecting connection 注册到 runtime 后 close awaiter 能正常恢复。

命令：

```bash
cd build
ctest -R "coroutine_runtime|connection_handle|event_bus|buffer_model|byte_buffer_reader|time_api|timer_lifecycle|async_facades|ipv6_dual_stack|base64|event_token|tcp_close_semantics" --output-on-failure
```

结果：

```text
100% tests passed, 0 tests failed out of 12
```

Benchmark：

```text
byte_buffer_append_copy        ops=200000       elapsed_ms=9.57029    ops_per_s=2.0898e+07     MiB_per_s=20408.2
buffer_chain_push_pop          ops=800000       elapsed_ms=30.6979    ops_per_s=2.60605e+07    MiB_per_s=12724.8
event_bus_publish              ops=300000       elapsed_ms=11.3775    ops_per_s=2.63677e+07
```

## P2 第二刀后

变更：

- 新增 `timer::TimerHandle`，作为对外 timer token/handle 兼容层。
- `NetworkRuntime` / `RuntimeView` 增加 `schedule_handle` / `schedule_periodic_handle` 和 `cancel_timer(TimerHandle)`。
- `AsyncConnectionContext` / `AsyncClientSession` / `AsyncDatagramClient` 对外 schedule API 改为返回 `TimerHandle`。
- `timer_lifecycle` 和 `async_facades` 测试迁移到 `TimerHandle` 路径。

命令：

```bash
cd build
ctest -R "coroutine_runtime|connection_handle|event_bus|buffer_model|byte_buffer_reader|time_api|timer_lifecycle|async_facades|ipv6_dual_stack|base64|event_token|tcp_close_semantics" --output-on-failure
```

结果：

```text
100% tests passed, 0 tests failed out of 12
```

Benchmark：

```text
byte_buffer_append_copy        ops=200000       elapsed_ms=9.94675    ops_per_s=2.01071e+07    MiB_per_s=19635.8
buffer_chain_push_pop          ops=800000       elapsed_ms=32.9408    ops_per_s=2.4286e+07     MiB_per_s=11858.4
event_bus_publish              ops=300000       elapsed_ms=11.3349    ops_per_s=2.6467e+07
```

## P6 第四刀后

变更：

- SSL 相关实现从 `net/secuity` 迁移到 `net/security`。
- core 与协议层 include 已改用 `net/security/*`。
- `net/secuity/*` 兼容 wrapper 已删除，core 侧仅保留新路径。

命令：

```bash
cd build
ctest -R "coroutine_runtime|connection_handle|event_bus|buffer_model|byte_buffer_reader|time_api|timer_lifecycle|async_facades|ipv6_dual_stack|base64|event_token|tcp_close_semantics" --output-on-failure
```

结果：

```text
100% tests passed, 0 tests failed out of 12
```

Benchmark：

```text
byte_buffer_append_copy        ops=200000       elapsed_ms=10.762     ops_per_s=1.85839e+07    MiB_per_s=18148.3
buffer_chain_push_pop          ops=800000       elapsed_ms=32.5617    ops_per_s=2.45687e+07    MiB_per_s=11996.4
event_bus_publish              ops=300000       elapsed_ms=13.2437    ops_per_s=2.26522e+07
```

结果：

```text
100% tests passed, 0 tests failed out of 12
```

Benchmark：

```text
byte_buffer_append_copy        ops=200000       elapsed_ms=9.80629    ops_per_s=2.03951e+07    MiB_per_s=19917.1
buffer_chain_push_pop          ops=800000       elapsed_ms=31.3977    ops_per_s=2.54795e+07    MiB_per_s=12441.2
event_bus_publish              ops=300000       elapsed_ms=11.1105    ops_per_s=2.70016e+07
```

## P2 第三刀后

变更：

- core legacy session facade 的 `schedule` / `cancel_timer` API 迁移到 `TimerHandle`。
- `StreamClientSession` / `DatagramClientSession` 的 `timeout_timer_` 成员迁移到 `TimerHandle`。

命令：

```bash
cd build
ctest -R "coroutine_runtime|connection_handle|event_bus|buffer_model|byte_buffer_reader|time_api|timer_lifecycle|async_facades|ipv6_dual_stack|base64|event_token|tcp_close_semantics" --output-on-failure
```

结果：

```text
100% tests passed, 0 tests failed out of 12
```

Benchmark：

```text
byte_buffer_append_copy        ops=200000       elapsed_ms=9.86393    ops_per_s=2.02759e+07    MiB_per_s=19800.7
buffer_chain_push_pop          ops=800000       elapsed_ms=30.9001    ops_per_s=2.58899e+07    MiB_per_s=12641.5
event_bus_publish              ops=300000       elapsed_ms=10.8862    ops_per_s=2.75577e+07
```
