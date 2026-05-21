# 脚本插件能力扩展方案

本文面向 Lua / TypeScript 脚本插件，目标是评估并规划“脚本侧直连宿主能力”的扩展顺序。

当前脚本插件已经具备的基础能力包括：
- 生命周期：`on_init` / `on_enable` / `on_disable` / `on_release`
- 日志：`logger`
- 事件：`event_bus` / `eventBus`
- 调度：`scheduler`
- 存储：`storage`
- 健康检查：`on_health_check`
- 资源追踪：`resource_guard`
- 宿主上下文：`app_name` / `plugin_name` / `config`

Lua 和 TypeScript 当前都已经通过宿主模块自动注册接入，Lua 侧说明见 `docs/plugins/LUA_PLUGIN_DESIGN.md`，TypeScript 侧与 Lua 共享同一套 `script` 插件机制。

## 1. 能力评估

### 1.1 `resource_guard`

结论：**优先完善，成本低**

现状：
- 已能查询资源追踪状态
- 已自动追踪事件订阅和调度任务

适合补充的接口：
- `tracked_count`
- `has_tracked_resources`
- `leak_report`
- `cleanup_plugin` 的只读调用入口

建议：
- 保持只读和调试用途为主
- 不要暴露过多底层清理细节给脚本

### 1.2 `service_catalog`

结论：**优先完善，成本中等**

现状：
- 宿主已经有服务目录抽象
- 主要是只读查询能力

适合补充的接口：
- `has_service`
- `list_services`
- `describe_service`
- `get_service_as_*` 风格的脚本友好封装

建议：
- 脚本侧返回脚本对象或 JSON 风格描述，不直接暴露 `std::any`
- 允许脚本发现宿主服务，但不要把 C++ 类型擦除结果直接透传给脚本

### 1.3 `service_registry`

结论：**可以做，但必须做成受限版，成本中高**

现状：
- C++ 侧已经支持插件注册受管服务
- 但脚本无法直接表达任意 C++ 服务对象

适合的方向：
- 脚本注册“脚本服务代理”或“脚本受管服务”
- 生命周期仍由宿主管理
- 服务对外暴露有限、稳定的脚本 API

不建议的方向：
- 直接让 Lua / TypeScript 注册任意 `std::any`
- 让脚本拿到可写的全局服务总表

### 1.4 `http_interceptor`

结论：**可以做，但最复杂，建议最后做**

现状：
- 宿主已经有中间件 / 路由拦截抽象
- 现有回调签名包含 `void *request` / `void *response`

为什么复杂：
- 需要为脚本构建请求对象、响应对象、路径、方法等包装层
- 需要处理取消、异常、超时、资源追踪、插件卸载回收
- 需要为 Lua 和 TypeScript 各自设计稳定的回调对象模型

建议：
- 单独设计脚本 HTTP API
- 不要直接把 `void *` 原样透传给脚本

### 1.5 `permission_guard`

结论：**只做只读，不做可写**

建议：
- 脚本可以知道自己有哪些权限
- 脚本不应直接 grant / revoke 自己的权限
- 权限仍由宿主策略控制

## 2. 推荐推进顺序

### P0

优先做：
- `resource_guard`
- `service_catalog`

目标：
- 先把“观测能力”和“只读发现能力”补齐
- 风险低，收益高

### P1

再做：
- `service_registry` 的受限版

目标：
- 允许脚本注册有限的宿主服务代理
- 生命周期、权限、卸载回收全部收口在宿主

### P2

最后做：
- `http_interceptor`

目标：
- 把脚本接入到 HTTP 请求链路
- 这部分最好单独做 API 设计和回归测试

## 3. 接口设计原则

1. **不直接暴露 `std::any`**
   - 脚本侧应该拿到对象、表、字符串或简单结构，而不是 C++ 类型擦除容器。

2. **默认只读，写操作受限**
   - `service_catalog` / `resource_guard` 更适合先开放。
   - `permission_guard` 不应开放写接口。

3. **生命周期继续由宿主统一管理**
   - 脚本侧新增能力必须纳入 `PluginLifecycleManager` 和 `PluginResourceGuard`。

4. **Lua / TypeScript 尽量统一语义**
   - API 命名可因语言习惯不同而不同，但语义要一致。
   - 例如 Lua 用 `event_bus`，TS 用 `eventBus`，但能力类型保持一致。

5. **所有可回调能力都必须可回收**
   - 订阅、定时任务、HTTP 拦截、服务代理都要能在卸载时自动清理。

## 4. 建议的测试补充

每补一类能力，至少补以下测试：
- 正常注册 / 查询 / 调用路径
- 权限不足时拒绝路径
- 插件卸载时自动清理
- 回调异常或超时时的回滚路径

## 5. 结论

如果目标是稳妥演进，建议按下面节奏推进：

1. 先补 `resource_guard` 和 `service_catalog`
2. 再做 `service_registry` 的受限版
3. 最后做 `http_interceptor`

这样可以先把“发现、调试、观测”能力做厚，再逐步进入“可写服务”和“HTTP 请求链路”这种高复杂度场景。
