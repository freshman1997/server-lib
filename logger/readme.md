## 设计目标

- 支持 `trace / debug / info / warn / error / fatal` 六个日志级别
- 支持控制台输出、文件输出、网络扩展输出
- 支持自定义格式化模式与时间格式
- 支持按天、按小时、按文件大小轮转
- 支持同步接口，并可扩展异步写入
- 默认以 UTF-8 处理日志消息，便于中文输出与跨平台落盘

## 组件说明

- `ConsoleLogger`
  负责控制台输出。Windows 下优先走 `WriteConsoleW`，避免 UTF-8 中文乱码。
- `FileLogger`
  负责文件落盘、轮转和备份清理。
- `Formatter`
  负责将 `LogItem` 按 pattern 渲染为最终字符串。
- `LogRegistry`
  提供全局默认 logger 和 `LOG_INFO` 等宏入口。
- `LoggerFactory`
  负责按配置创建不同类型的 logger，并支持扩展 logger 注入。
- `NetLogger`
  通过 `Core` 网络层异步连接远端日志服务，支持排队发送、自动重连和队列溢出策略。

## 快速示例

```cpp
#include "logger.h"

int main()
{
    LOG_INFO("server started on port {}", 8080);
    LOG_WARN("中文日志测试: {}", "你好，世界");
    LOG_ERROR("request failed: code={}, msg={}", 500, "internal error");
    return 0;
}
```

## 网络日志配置

`NetLogger` 相关配置项：

- `net_server_ip`
  远端日志服务地址。
- `net_server_port`
  远端日志服务端口。
- `net_auto_reconnect`
  是否在断连后自动重连。
- `net_connect_timeout_ms`
  单次连接超时时间，单位毫秒。
- `net_reconnect_delay_ms`
  两次自动重连之间的等待时间，单位毫秒。
- `net_max_retries`
  最大重试次数，`-1` 表示无限重试。
- `net_max_pending_messages`
  本地待发送消息队列上限，防止远端长期不可用时内存持续增长。
- `net_drop_oldest_on_overflow`
  队列满时是否丢弃最旧消息；为 `false` 时丢弃最新消息。

示例：

```json
{
  "log_level": "info",
  "net_server_ip": "127.0.0.1",
  "net_server_port": 9999,
  "net_auto_reconnect": true,
  "net_connect_timeout_ms": 1000,
  "net_reconnect_delay_ms": 250,
  "net_max_retries": -1,
  "net_max_pending_messages": 10000,
  "net_drop_oldest_on_overflow": true,
  "fmt": "{asctime} [{levelname}] {message}"
}
```

## 编码约定

- 日志消息默认按 UTF-8 处理
- Windows 真控制台会转换为 UTF-16 输出
- 管道、重定向、IDE 终端场景会保留 UTF-8 字节序列
- 建议源码文件统一保存为 UTF-8
