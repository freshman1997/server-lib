# 插件系统设计文档

## 1. 文档目标

本文档用于指导当前项目插件系统的后续重构与演进，重点回答以下问题：

- 插件系统的目标边界是什么
- 进程内插件如何尽量保证宿主稳定
- 插件 SDK 应如何稳定收口
- 事件、服务、资源能力如何正式建模
- 后续应按什么阶段推进，哪些能力先做，哪些能力后做

本文档面向当前项目现状，默认约束如下：

- 当前项目以 `C++` 原生插件为主
- 当前插件运行模式以 `in-process` 为主
- 项目仍处于重构期，不要求兼容旧插件 ABI
- 目标平台优先 `MinGW / Linux / macOS`

## 2. 当前现状

当前插件主线已经具备基础能力：

- 插件可通过 `PluginContext` 获取宿主能力视图
- 插件服务注册已经具备 typed contract 雏形
- 插件权限、资源清理、调度、存储、日志等宿主接口已经基本成型
- 插件宿主 smoke test 已能稳定通过

当前已经落地的关键接口主要包括：

- `PluginContext`
- `HostServiceRegistry`
- `HostServiceCatalog`
- `HostResourceGuard`
- `HostPermissionGuard`
- `HostScheduler`
- `HostStorage`

但从“主流插件系统”的标准看，当前仍有三个关键缺口：

1. 缺少正式的故障隔离与故障状态机  
   现在更像“功能可用”，还不是“宿主稳定优先”的插件平台。

2. 缺少真正稳定的 SDK boundary  
   虽然已有 `PluginContext` 和 typed service contract，但还需要进一步收敛成可长期维护的宿主契约。

3. 缺少能力模型和治理策略  
   事件、服务、资源、HTTP 扩展、存储等能力已经存在，但还没有统一的授权、声明、审计、回收模型。

## 3. 设计目标

插件系统的核心目标不是“让插件什么都能做”，而是：

- 让插件能扩展宿主
- 让宿主能约束插件
- 让插件出问题时，宿主仍然尽可能活着
- 让插件能力边界清晰、可测试、可演进

具体目标如下：

- 稳定性优先：插件异常不能轻易击穿宿主主流程
- 契约优先：插件与宿主的交互必须通过明确接口，而不是侵入内部实现
- 生命周期可控：插件注册的事件、任务、服务、路由、存储句柄必须可统一清理
- 能力可声明：插件需要什么权限、服务、资源类型，应显式可见
- 演进可持续：后续可平滑引入进程外插件或沙箱插件

## 4. 非目标

当前阶段不作为主目标的事项：

- 不追求旧插件 ABI 兼容
- 不立即实现完整沙箱
- 不立即支持任意脚本语言插件
- 不承诺“进程内插件绝不会导致宿主崩溃”

最后这一点需要明确：  
只要插件是 `in-process C++ plugin`，就不可能像 `out-of-process` 一样从根上隔离非法内存访问、ABI 破坏、死锁和未定义行为。

## 5. 参考主流插件系统的设计原则

主流插件系统大致都遵循以下分层思路：

### 5.1 第一层：契约边界

插件只能通过稳定的宿主接口工作，不允许直接依赖宿主内部对象和实现细节。

典型表现：

- 有明确的 host API version
- 有稳定的 plugin manifest / metadata
- 有能力声明和权限声明
- 有 typed service / extension point contract

### 5.2 第二层：生命周期治理

插件不是简单的“加载/卸载”，而是带状态机的受控对象。

常见状态包括：

- `discovered`
- `loaded`
- `initialized`
- `active`
- `degraded`
- `faulted`
- `quarantined`
- `stopped`
- `unloaded`

### 5.3 第三层：资源托管

插件注册的所有宿主侧资源都必须由宿主管控并可统一回收。

包括但不限于：

- 事件订阅
- scheduler task
- coroutine task
- async task
- HTTP route / middleware
- service registration
- storage session
- network callback

### 5.4 第四层：故障隔离

主流做法通常分两级：

- 低成本方案：进程内插件 + 边界捕获 + 资源切断 + 自动禁用
- 强隔离方案：进程外插件 + IPC/RPC + 崩溃隔离

当前项目更适合先把第一层做到扎实，再为第二层预留扩展位。

## 6. 推荐总体架构

推荐将插件系统分成 5 层：

```text
Plugin Package / Manifest
        |
Plugin Loader / Symbol Solver
        |
Plugin Lifecycle Manager
        |
Plugin Host Runtime
        |
Stable Plugin SDK Boundary
```

对应职责如下：

### 6.1 Plugin Package / Manifest

负责描述插件的静态信息：

- `plugin_id`
- `name`
- `version`
- `api_version`
- `entry`
- `permissions`
- `dependencies`
- `extension_points`
- `run_mode`

建议后续统一形成正式 manifest 结构，而不是零散 metadata。

### 6.2 Plugin Loader / Symbol Solver

负责：

- 加载动态库
- 校验入口符号
- 校验 API version
- 生成插件实例

这一层只负责“能不能装载”，不负责“能不能安全运行”。

### 6.3 Plugin Lifecycle Manager

负责：

- 插件状态流转
- 初始化、启动、停止、卸载顺序
- 故障转移
- 自动禁用 / quarantine 策略

这是插件系统治理的核心层。

### 6.4 Plugin Host Runtime

负责：

- 事件总线接入
- 调度器接入
- 服务目录与服务注册
- 存储接入
- 资源跟踪与清理
- 权限校验
- HTTP 扩展点接入

这一层要做成“插件能力托管器”。

### 6.5 Stable Plugin SDK Boundary

这是插件唯一应依赖的宿主交互层。

建议插件只能看到：

- `PluginContext`
- `PluginConfigView`
- `PluginService`
- `Host*` 系列抽象接口
- 明确的事件 descriptor / service descriptor / resource type

不应让插件直接拿到宿主内部对象、具体实现类或可变全局状态。

## 7. 推荐生命周期状态机

建议把插件生命周期正式定义为：

```text
discovered
  -> loaded
  -> initialized
  -> active
  -> degraded
  -> faulted
  -> quarantined
  -> stopping
  -> stopped
  -> unloaded
```

推荐语义如下：

- `discovered`
  - 宿主发现插件包，但未装载

- `loaded`
  - 动态库已装载，符号已解析

- `initialized`
  - 插件对象已创建，基础上下文已建立

- `active`
  - 插件已开始提供功能

- `degraded`
  - 插件仍可存活，但部分能力已关闭

- `faulted`
  - 插件运行中出现故障，已停止接受新入口

- `quarantined`
  - 插件被熔断隔离，本次进程生命周期不再自动恢复

- `stopping`
  - 正在回收资源并执行 stop/release

- `stopped`
  - 宿主已切断插件运行时入口

- `unloaded`
  - 动态库已卸载

## 8. 故障模型与宿主保护策略

这是插件系统最关键的一部分。

### 8.1 要防的故障类型

建议至少区分以下几类：

1. 可恢复逻辑故障
- 回调抛异常
- 服务注册失败
- 配置错误
- 插件返回非法状态

2. 资源型故障
- 定时任务泄漏
- 事件订阅未注销
- 反复注册路由
- 存储句柄泄漏

3. 性能型故障
- 插件长时间阻塞 event loop
- 高频失败重试
- 异常日志风暴

4. 致命故障
- 非法内存访问
- ABI 不匹配
- 死锁
- 破坏宿主核心状态

### 8.2 宿主保护分层

建议按以下顺序设计：

#### 层 1：边界捕获

所有宿主调用插件的入口都要统一进入 guard：

- `on_load`
- `on_enable`
- `on_config_changed`
- 事件回调
- 定时任务回调
- HTTP 扩展回调
- 服务回调

guard 至少负责：

- 捕获异常
- 记录 fault event
- 累计故障计数
- 判断是否进入 `degraded` 或 `faulted`

#### 层 2：入口切断

当插件进入 `faulted` 时，宿主必须立即停止向该插件派发新工作：

- 不再分发事件
- 不再执行 scheduler callback
- 不再暴露其服务
- 不再走其 HTTP route / middleware
- 不再保留存活中的 coroutine / async 回调入口

#### 层 3：资源回收

依赖 `HostResourceGuard` 统一回收：

- event subscription
- scheduler task
- async/coroutine task
- route install
- callback binding

这一层是防止“插件逻辑停了，但资源还活着”。

#### 层 4：熔断与隔离

建议引入：

- 单次故障：记录并观察
- 短时间多次故障：进入 `faulted`
- 高频重复故障：进入 `quarantined`

### 8.3 是否自动卸载

建议结论：

- 可以自动禁用
- 可以自动隔离
- 自动卸载应谨慎

原因：

1. 自动卸载不是最安全的第一步  
   插件可能还有未回收回调、跨模块对象、未完成任务，直接 `dlclose` 风险很高。

2. 更合理的顺序应该是：

```text
fault detected
-> mark faulted
-> cut off new entries
-> reclaim resources
-> unregister services/routes/subscriptions
-> stop plugin
-> decide disable / quarantine / unload
```

3. 对当前项目来说，建议策略为：

- 默认自动进入 `faulted`
- 完成资源回收后进入 `stopped`
- 默认本次运行内不自动重新加载
- 重复故障插件进入 `quarantined`
- `unload` 作为显式操作或后续受控策略

## 9. SDK Boundary 设计建议

当前项目已经有 `PluginContext` 雏形，建议正式收口为以下原则。

### 9.1 插件只依赖稳定抽象

插件可依赖：

- `PluginContext`
- `PluginConfigView`
- `PluginService`
- `HostEventBus`
- `HostLogger`
- `HostServiceCatalog`
- `HostServiceRegistry`
- `HostPermissionGuard`
- `HostResourceGuard`
- `HostScheduler`
- `HostHttpInterceptor`
- `HostStorage`

插件不应依赖：

- 宿主具体实现类
- 宿主内部容器结构
- 宿主线程模型细节
- 非稳定内部头文件

### 9.2 SDK Boundary 需要版本化

建议保留并强化：

- `host_api_version`
- `plugin api version`
- `service contract version`

后续所有 extension point 都应带 version。

### 9.3 PluginContext 应保持“只读 + 能力门面”

`PluginContext` 不应持续膨胀成“大杂烩对象”，建议定位为：

- 静态元数据视图
- 能力快照
- 少量高频 helper

更复杂的行为应下沉到宿主接口中。

## 10. 服务能力模型

建议把服务分成三类：

### 10.1 Host Service

由宿主提供给插件使用，例如：

- logger service
- http service
- metrics service
- config service

建议 descriptor 包含：

- `name`
- `type_name`
- `contract_id`
- `contract_version`
- `scope`
- `lifetime`
- `required_permissions`

### 10.2 Plugin Service

由插件注册给宿主或其他插件使用。

建议 descriptor 包含：

- `name`
- `plugin_name`
- `type_name`
- `contract_id`
- `contract_version`
- `managed_lifecycle`
- `visibility`

### 10.3 Internal Service

仅宿主内部可见，不进入稳定 SDK。

这类服务不应暴露给插件。

## 11. 事件能力模型

建议正式区分两种事件：

### 11.1 Lifecycle Event

插件与宿主生命周期相关：

- plugin loaded
- plugin load failed
- plugin activated
- plugin degraded
- plugin faulted
- plugin quarantined
- plugin stopped
- plugin unloaded

### 11.2 Domain Event

业务或系统事件，例如：

- http request handled
- connection opened
- service registered
- config changed

建议事件 descriptor 至少包含：

- `name`
- `category`
- `payload_type`
- `scope`
- `delivery_semantics`
- `required_permission`

同时建议提前明确事件分发原则：

- 默认进程内异步
- 允许同步少量核心生命周期事件
- 宿主保留背压与限流能力

## 12. 资源能力模型

建议统一 `PluginResourceType`，并继续扩展为正式治理模型。

当前资源应至少覆盖：

- `event_subscription`
- `scheduler_task`
- `coroutine_task`
- `async_task`
- `callback`
- `http_route`
- `middleware`
- `service_registration`
- `storage_session`

资源模型必须支持：

- track
- untrack
- bulk cleanup by plugin
- cleanup failure logging
- resource leak reporting

推荐后续增加：

- resource snapshot
- resource quota
- per-plugin leak report

## 13. 权限与能力控制模型

建议将“权限”与“能力可用性”分开建模。

### 13.1 权限

权限描述“插件被允许做什么”，例如：

- 访问网络
- 读写存储
- 注册 HTTP 扩展
- 发布订阅事件
- 注册服务

### 13.2 能力可用性

能力可用性描述“宿主当前是否提供该能力”，例如：

- 当前 worker 进程不允许装 HTTP route
- 当前运行模式下无 scheduler
- 存储后端未初始化

插件最终能否调用某能力，应该由：

```text
permission granted
AND
host capability available
AND
runtime state healthy
```

共同决定。

## 14. 运行模式建议

插件系统应兼容当前项目的运行模式演进：

- `single_thread`
- `multi_thread`
- `multi_process`

建议原则：

### 14.1 single_thread

- 插件回调必须避免阻塞 event loop
- 可作为开发和调试基线

### 14.2 multi_thread

- 明确插件回调线程归属
- 不允许插件假设所有回调都来自同一线程
- Host SDK 中涉及线程切换的能力要显式表达

### 14.3 multi_process

- 当前先把 `PluginRunMode` 语义补清晰
- 后续可将高风险插件迁移到独立进程

## 15. 推荐演进路线

建议分 4 个阶段推进。

### 阶段 A：收口稳定 SDK

目标：

- 固化 `PluginContext`
- 固化 service/event/resource descriptor
- 固化 host api version 与 contract version 语义

完成标准：

- 插件不再直接依赖宿主内部实现
- 示例插件全部迁到新 SDK
- 旧式裸接口不再新增使用

### 阶段 B：补齐生命周期与故障治理

目标：

- 引入正式插件状态机
- 引入 fault/degraded/quarantine 策略
- 所有宿主到插件入口都统一 guard

完成标准：

- 插件异常不会继续穿透宿主主流程
- 插件进入 fault 后可自动切断入口
- 插件资源能可靠回收

### 阶段 C：补齐能力治理与测试矩阵

目标：

- 补齐权限检查
- 补齐 service/event/resource 回归测试
- 输出插件健康报告和资源泄漏报告

完成标准：

- 有正式 capability enforcement tests
- 有 descriptor compatibility tests
- 有 fault injection tests

### 阶段 D：预留强隔离扩展

目标：

- 设计进程外插件协议
- 将高风险插件与高性能插件分层

完成标准：

- 当前 in-process SDK 可映射到后续 IPC boundary
- 宿主架构上允许引入 sandbox plugin host

## 16. 近期优先任务建议

如果按当前项目节奏推进，建议优先级如下：

1. 定义正式插件状态机与 fault policy
2. 统一 host->plugin 调用 guard
3. 将 service registration / event subscription / scheduler task 全部纳入强制资源跟踪
4. 为 HTTP route / middleware 补资源类型和可回收模型
5. 增加插件 fault injection 测试
6. 增加 capability enforcement 测试
7. 增加 descriptor compatibility 测试

## 17. 完成判定

插件系统可以认为“基础重构完成”，应至少满足以下条件：

- 插件只依赖稳定 SDK boundary
- 服务、事件、资源能力都有正式 descriptor
- 所有宿主到插件入口都有统一保护
- 插件进入 fault 后可自动切断入口并回收资源
- smoke test 与 fault injection test 均稳定通过
- 示例插件全部迁移到新契约

如果要进一步称为“生产级插件平台”，还需要再满足：

- 完整权限治理
- 资源配额和泄漏报告
- 健康监控
- 更强的隔离模型，至少具备进程外扩展路线

## 18. Lua 脚本插件支持

在完成阶段 A-D 的 C++ 原生插件体系重构后，新增了 Lua 脚本插件支持，使插件系统具备多语言扩展能力。

### 18.1 架构

脚本插件系统采用**语言无关基类 + 语言模块注册**的模块化架构：

```text
PluginManager::load("my_lua_plugin")
  │
  ├─ 读取 config (plugin_name.json 或 plugin_name/plugin.json)
  │    └─ run_mode = "script" → ScriptPluginRegistry 路径
  │    └─ 其他 → 原有 native 路径（不变）
  │
  └─ ScriptPluginRegistry::create(language, manifest, config)
       │
       ├─ 按 language 查找注册的工厂函数
       │    └─ "lua" → LuaScriptPluginAdapter 工厂
       │    └─ 其他语言 → (未来扩展)
       │
       └─ 工厂创建语言特定适配器（继承 ScriptPluginAdapter → Plugin）
            ├─ LuaScriptPluginAdapter 内部持有 lua_State*
            │    ├─ 受内存预算限制的自定义分配器
            │    ├─ 生命周期回调桥接到 Lua 函数（受指令计数 hook 保护）
            │    ├─ 宿主能力通过 userdata+元表 绑定到 Lua
            │    ├─ EventBus/Scheduler 回调同样受指令计数 hook 保护
            │    └─ Lua 沙箱移除 io/os危险函数/debug/package.loadlib/package.cpath/C searchers
            └─ 释放时先 stop()（ResourceGuard 清理 + on_release + lua_close）再 unload()（delete adapter）
```

### 18.2 关键组件

**语言无关层（plugin_core）：**

- `ScriptPluginAdapter` — 纯虚基类，Template Method 模式，不依赖任何特定脚本语言
- `ScriptPluginRegistry` — 单例注册机制，管理 `language → FactoryFn` 映射

**Lua 实现层（plugin_lua，可选模块）：**

- `LuaScriptPluginAdapter` — 继承 `ScriptPluginAdapter`，实现所有 Lua 特定逻辑
- `LuaScriptPluginAdapter::Config` — 支持 `memory_budget_bytes` 和 `max_instructions_per_call` 配置
- `LuaMemoryBudget` + `lua_budget_allocator` — 自定义内存分配器，限制 Lua 内存使用
- `lua_sethook` + `LUA_MASKCOUNT` — 指令计数 hook，防止死循环（覆盖生命周期回调和回调 dispatch）
- `lua_register_host_modules()` — 将宿主能力（Logger/EventBus/Scheduler/Storage/ResourceGuard）绑定到 Lua
- `init_lua_plugin_module()` — 注册 Lua 工厂到 `ScriptPluginRegistry`，宿主程序启动时调用

**宿主侧（plugin_core）：**

- `PluginManager::load_script_plugin()` — 根据 run_mode=script 通过 registry 创建适配器

### 18.3 设计原则

- **不破坏现有 C++ 插件路径** — Lua 插件是新增分支，不是替代
- **复用现有治理体系** — 状态机、CallGuard、ResourceGuard、PermissionGuard 对 Lua 插件一视同仁
- **Lua 侧安全优于 C++ 侧** — lua_pcall 天然异常隔离 + 沙箱
- **双重保护** — lua_pcall (Lua 侧) + PluginCallGuard (C++ 侧)
- **语言可扩展** — `ScriptPluginRegistry` 支持注册任意语言适配器，Lua 是第一个实现

### 18.4 模块化结构

```text
plugins/core/                                  # 语言无关抽象层
  include/plugin/
    script_plugin_adapter.h                    # 纯虚基类
    script_plugin_registry.h                   # 注册机制
  src/plugin/
    script_plugin_adapter.cpp                  # 基类实现
    script_plugin_registry.cpp                 # 注册实现

plugins/lua/                                   # Lua 特定实现（可选模块）
  include/
    lua_script_plugin_adapter.h                # Lua 适配器
    lua_plugin_module.h                        # 模块注册入口
  src/
    lua_script_plugin_adapter.cpp              # Lua 适配器实现
    lua_host_bindings.h/.cpp                   # 宿主能力绑定
    lua_plugin_module.cpp                      # 注册 Lua 工厂
  CMakeLists.txt                               # 链接 lua_static + PluginCore
```

### 18.5 Lua 插件定义

Lua 插件通过 `plugin.json` 声明自身，脚本必须返回 `plugin` 表，`on_init` 是唯一必须的函数：

```lua
local plugin = {}
function plugin.on_init(ctx)
    ctx.logger:info("hello from Lua!")
    return true
end
function plugin.on_release() end
return plugin
```

### 18.6 宿主能力绑定

一期已绑定：Logger / EventBus / Scheduler / Storage / ResourceGuard

二期规划：ServiceRegistry / ServiceCatalog / HttpInterceptor

### 18.7 启用 Lua 支持

宿主程序需要：
1. 链接 `PluginLua` 模块
2. 在启动时调用 `init_lua_plugin_module()` 注册 Lua 工厂

此后 `PluginManager::load_script_plugin()` 会自动通过 registry 找到 Lua 适配器。

详细设计见 `docs/LUA_PLUGIN_DESIGN.md`。

## 19. 总结

当前项目插件系统已经从“能加载插件”进入到“开始形成插件平台骨架”的阶段。  
下一步最重要的不是继续堆功能，而是把以下三件事做硬：

- 稳定 SDK boundary
- 正式生命周期与 fault policy
- 事件 / 服务 / 资源能力模型

这三件事做实之后，插件系统才真正能成为项目基座，而不只是一个可运行的扩展模块。
