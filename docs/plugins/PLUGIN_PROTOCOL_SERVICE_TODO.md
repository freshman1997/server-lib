# Plugin Protocol Service TODO

日期：2026-05-17

## 目标

把当前的插件协议服务从“宿主内置 echo proof”推进到“插件可以真正定义网络协议处理逻辑”。

最终希望插件可以声明一个 TCP/UDP 协议服务，由宿主负责端口、worker、listener、权限、资源治理和生命周期，插件只实现协议业务逻辑。

## 当前状态

已完成：

- [x] `plugin.json` 支持 `protocol_services` 声明。
- [x] 权限系统支持 `register_protocol_service`。
- [x] `PluginManager::discover_protocol_services()` 可以在插件完整加载前扫描协议服务声明。
- [x] 插件协议服务可以转换为 `core/app::ServiceDescriptor`。
- [x] `PluginProtocolServiceAdapter` 可以在 worker-local runtime 中初始化插件宿主。
- [x] 每个 worker-local 插件协议服务实例拥有独立 `PluginManager`，避免覆盖彼此的 `PluginContext`。
- [x] TCP echo proof 已落地：`type = "echo"` 时可绑定 worker-owned TCP listener 并完成真实 roundtrip。

当前主要限制：

- 协议处理逻辑已从 adapter 的 `run_echo_protocol()` 移到 stream handler registry；`type = "echo"` 现在映射到内置 demo handler `builtin.echo`。
- `PluginProtocolServiceAdapter` 目前只支持 `transport = "tcp"`，framing 支持 `raw` / `line`；UDP 和脚本侧连接对象仍待实现。
- `HostNetworkRuntime` 只暴露 timer / dispatch，不暴露 listener、connection、read/write。
- Lua / TypeScript 插件还不能拿到连接对象处理字节流。
- UDP/datagram 协议服务还没有 handler SDK。

## 目标形态

### Manifest

扩展 `protocol_services` 声明，让插件能描述 handler、传输、framing 和资源限制。

建议字段：

```json
{
  "protocol_services": [
    {
      "name": "line_echo",
      "type": "custom",
      "transport": "tcp",
      "host": "127.0.0.1",
      "port": 9000,
      "handler": "main.on_connection",
      "framing": "line",
      "read_timeout_ms": 30000,
      "idle_timeout_ms": 60000,
      "write_timeout_ms": 30000,
      "max_connections": 1024,
      "max_frame_bytes": 65536,
      "contract_id": "plugin.line_echo",
      "contract_version": 1
    }
  ]
}
```

第一阶段只要求 `transport = "tcp"`；UDP 可以作为后续阶段。

### C++ SDK

新增插件侧网络协议抽象，避免把 core/net 具体类型直接暴露给插件。

建议接口：

- `HostStreamConnection`
  - `id()`
  - `peer_address()`
  - `local_address()`
  - `read(...)`
  - `write(...)`
  - `flush(...)`
  - `close(...)`
  - `is_open()`
- `PluginStreamProtocolHandler`
  - `on_accept(HostStreamConnection&)`
  - `on_data(HostStreamConnection&, ByteSpan)`
  - `on_close(HostStreamConnection&)`
  - `on_error(HostStreamConnection&, ErrorInfo)`
- `PluginProtocolHandlerRegistry`
  - 插件加载时注册 handler factory
  - `PluginProtocolServiceAdapter` 按 manifest 的 `handler` / `type` 查找 handler

### Script SDK

Lua / TypeScript 应该拿到脚本友好的连接对象。

Lua 示例：

```lua
local plugin = {}

function plugin.on_connection(conn)
    while conn:is_open() do
        local data = conn:read_line(30000)
        if not data then break end
        conn:write(data .. "\n")
        conn:flush()
    end
end

return plugin
```

TypeScript 示例：

```ts
export function onConnection(conn) {
  for (;;) {
    const line = conn.readLine(30000);
    if (line == null) break;
    conn.write(line + "\n");
    conn.flush();
  }
}
```

## 分阶段任务

### P0：收紧当前 echo proof

- [x] 把 `type = "echo"` 明确标记为内置示例 handler，不要让它看起来像完整自定义协议能力。
- [x] 在 `PluginProtocolServiceAdapter` 日志中区分 manifest 校验失败、handler 不存在、bind 失败、runtime 缺失。
- [x] 为 unsupported `protocol/type` 增加负向测试。
- [x] 文档说明当前 echo proof 的边界。

### P1：协议服务 manifest 扩展

- [x] 扩展 `ProtocolServiceDescriptor` 字段：`transport`、`handler`、`framing`、timeout、连接数、frame 大小。
- [x] 保持旧字段 `protocol` 兼容，短期映射到 `transport`。
- [x] manifest parser 增加默认值、范围校验和错误日志。
- [x] endpoint planning 只消费 listener 相关字段，不理解 handler 细节。
- [x] 增加 manifest parsing 单元测试。

### P2：C++ 自定义 stream handler

- [x] 新增 `HostStreamConnection` 插件 SDK 接口。
- [x] 新增 `PluginStreamProtocolHandler` 或等价 handler interface。
- [x] 新增 `PluginProtocolHandlerRegistry`。
- [x] `PluginProtocolServiceAdapter` 从 hard-coded echo 改为 registry lookup。
- [x] 保留内置 echo handler 作为 registry 中的默认 demo handler。
- [x] 增加 C++ 插件样例：line echo 或 length-prefixed echo。
- [x] 增加 C++ 插件协议 roundtrip 回归测试。

### P3：连接生命周期和资源治理

- [ ] 每个 listener 和 active connection 都进入 `HostResourceGuard` 跟踪。
- [x] 插件停用/卸载时关闭 listener 和 active connections。
- [ ] 连接回调经过 `PluginCallGuard`，handler 抛错/返回失败时关闭连接并上报 fault event。
- [x] 加入 per-service 连接上限和拒绝策略。
- [ ] 加入 read/write/idle timeout。
- [ ] 加入 write buffer 上限和 backpressure 策略。
- [ ] 增加停用时 active connection 自动关闭的回归测试。

### P4：framing 层

- [x] 支持 `raw` framing。
- [x] 支持 `line` framing。
- [ ] 支持 `length_prefixed` framing。
- [x] 支持 `max_frame_bytes`，超限关闭连接并上报。
- [ ] 将 framing 错误和 plugin handler 错误分开统计。
- [ ] 增加半包、粘包、大包、超限包测试。

### P5：Lua 绑定

- [ ] 为 Lua 暴露 `HostStreamConnection` userdata。
- [ ] 绑定 `read` / `read_line` / `write` / `flush` / `close` / `is_open`。
- [ ] Lua 回调执行沿用现有内存预算和指令计数限制。
- [ ] Lua callback 引用进入 ResourceGuard，插件释放时清理。
- [ ] 增加 Lua TCP line echo 示例插件。
- [ ] 增加 Lua 协议服务 roundtrip、handler error、timeout 测试。

### P6：TypeScript 绑定

- [ ] 为 QuickJS/TypeScript 暴露连接对象。
- [ ] 绑定 `read` / `readLine` / `write` / `flush` / `close`。
- [ ] JS callback 引用进入 ResourceGuard，插件释放时清理。
- [ ] 增加 TypeScript 协议服务示例。
- [ ] 增加 TypeScript 协议服务 smoke test。

### P7：UDP/datagram 协议服务

- [ ] 新增 `HostDatagramEndpoint`。
- [ ] 新增 `PluginDatagramProtocolHandler`。
- [ ] manifest 支持 `transport = "udp"`。
- [ ] 支持 `on_datagram(peer, bytes)` 和 `send_to(peer, bytes)`。
- [ ] 增加 UDP echo roundtrip 测试。
- [ ] 增加 per-peer 状态和 idle cleanup 策略。

### P8：权限和安全边界

- [ ] 将粗粒度权限拆细：
  - [ ] `listen_tcp`
  - [ ] `listen_udp`
  - [ ] `open_outbound_connection`
  - [ ] `bind_privileged_port`
  - [ ] `use_tls`
- [ ] 默认禁止绑定 privileged port，除非显式授权。
- [ ] 限制 `host = 0.0.0.0` 的使用策略。
- [ ] 明确 outbound connect 是否属于本阶段范围。
- [ ] 对每个插件协议服务记录 capability snapshot。

### P9：观测和治理

- [ ] 增加 protocol service started/stopped/bind_failed/connection_accepted/connection_closed/faulted 事件。
- [ ] 暴露每个插件协议服务的连接数、读写字节数、错误数。
- [ ] 日志带上 plugin、service、worker、service_instance。
- [ ] 增加健康检查：listener 是否存在、连接数是否超限、handler fault 计数。

### P10：文档和验收

- [x] 更新 `PLUGIN_SYSTEM_DESIGN.md` 中的协议服务章节。
- [x] 增加 C++ 插件协议示例 README。
- [ ] 增加 Lua 插件协议示例 README。
- [x] 增加 manifest 字段参考表。
- [x] 增加迁移说明：当前 `type = "echo"` demo 到自定义 handler 的迁移方式。

## 最小可交付闭环

第一轮真正可称为“插件自定义 TCP 协议”至少要满足：

- [x] 插件 manifest 可以声明 `handler`。
- [x] 宿主不再只根据 `type = "echo"` hard-code handler。
- [x] C++ 插件可以实现一个自定义 TCP line protocol。
- [ ] Lua 插件可以实现一个 TCP echo/line protocol。
- [ ] 插件卸载时 listener 和 active connections 被自动关闭。
- [ ] handler 异常不会打穿宿主，并能产生 fault event。
- [ ] multi-worker + `reuse_port` 下每个 worker 拥有独立 plugin context 和 listener。

## 推荐落地顺序

1. 先做 C++ SDK 和 registry，把 hard-coded echo 改成 registry demo handler。
2. 再做 `HostStreamConnection` 的 Lua 绑定，跑通 Lua line echo。
3. 然后补生命周期治理、framing、连接上限和 timeout。
4. 最后做 TypeScript 和 UDP。

这样能最快从“宿主内置 echo proof”变成“插件真的能写协议逻辑”，同时把风险控制在 TCP stream 一条主线上。
