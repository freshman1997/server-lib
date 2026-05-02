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
