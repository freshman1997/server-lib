# Lua 脚本插件设计文档

## 1. 目标

在现有 C++ 原生插件体系基础上，支持以 Lua 脚本编写插件，使插件系统具备多语言扩展能力。

核心原则：

- **不破坏现有 C++ 插件路径** — Lua 插件是新增分支，不是替代
- **复用现有治理体系** — 状态机、CallGuard、ResourceGuard、PermissionGuard 对 Lua 插件一视同仁
- **Lua 侧安全优于 C++ 侧** — `lua_pcall` 天然异常隔离，非法内存访问不可能发生

## 2. 架构总览

```text
PluginManager::load("my_lua_plugin")
  │
  ├─ 读取 manifest (plugin.json 或内嵌)
  │    └─ run_mode = "script"
  │    └─ entry = "main.lua"
  │    └─ language = "lua"
  │
  ├─ run_mode == native → 原有 dlsym 路径（不变）
  │
  └─ run_mode == script → ScriptPluginRegistry 路径（新增）
       │
       ├─ 按 language 查找注册的工厂函数
       │    └─ "lua" → LuaScriptPluginAdapter 工厂
       │    └─ "javascript" → (未来) JavaScript 工厂
       │
       ├─ 工厂创建语言特定适配器（继承 ScriptPluginAdapter）
       │    └─ LuaScriptPluginAdapter 内部持有 lua_State*
       │    └─ 生命周期回调桥接到 Lua 函数
       │
       ├─ 加载脚本文件
       │    └─ 从 plugin_root_path + entry 拼接路径
       │
       └─ 适配器内部注册宿主能力模块
            └─ host.logger
            └─ host.event_bus
            └─ host.scheduler
            └─ host.storage
            └─ host.resource_guard
            └─ host.service_catalog  (二期)
            └─ host.http_interceptor (二期)
```

## 3. PluginManifest 扩展

### 3.1 PluginRunMode 新增 script

```cpp
enum class PluginRunMode {
    unknown,
    single_thread,
    multi_thread,
    multi_process,
    sandbox,
    script,       // 新增：脚本语言插件
};
```

### 3.2 PluginManifest 新增字段

```cpp
struct PluginManifest {
    // ... 现有字段不变 ...
    std::string entry;              // 脚本入口文件 (如 "main.lua")
    std::string language;           // 脚本语言标识 (如 "lua", 预留)
    PluginRunMode run_mode;         // script 表示脚本插件
};
```

### 3.3 Lua 插件的配置文件

Lua 插件不需要编译 `.so`，而是通过 `plugin.json` 声明自身：

```json
{
    "plugin_id": "lua_greeter",
    "name": "LuaGreeter",
    "version": "1.0.0",
    "author": "yuan",
    "description": "A demo Lua script plugin",
    "api_version": 1,
    "entry": "main.lua",
    "language": "lua",
    "run_mode": "script",
    "permissions": "use_event_bus,use_logger,use_scheduler,use_storage",
    "depends_on": []
}
```

## 4. ScriptPluginAdapter（语言无关基类）

### 4.1 类设计

`ScriptPluginAdapter` 是一个纯虚基类，采用 Template Method 模式，不依赖任何特定脚本语言：

```cpp
class ScriptPluginAdapter : public Plugin
{
public:
    explicit ScriptPluginAdapter(PluginManifest manifest);
    ~ScriptPluginAdapter() override;

    void on_loaded() override;
    bool on_init(const PluginContext &context) override;
    void on_enable() override;
    void on_disable() override;
    void on_release() override;
    bool on_health_check() const override;
    void on_config_changed(const PluginConfigView &config) override;
    PluginMeta meta() const override;
    PluginManifest manifest() const override;

    virtual bool load_script(const std::string &script_path) = 0;

protected:
    virtual bool do_init(const PluginContext &context) = 0;
    virtual void do_enable() {}
    virtual void do_disable() {}
    virtual void do_release() {}
    virtual bool do_health_check() const { return true; }
    virtual void do_config_changed(const PluginConfigView & /*config*/) {}

    PluginManifest manifest_;
    PluginContext context_;
    bool script_loaded_ = false;
};
```

### 4.2 ScriptPluginRegistry（语言模块注册机制）

`ScriptPluginRegistry` 是单例，管理脚本语言适配器的注册与创建：

```cpp
class ScriptPluginRegistry
{
public:
    using FactoryFn = std::function<ScriptPluginAdapter*(const PluginManifest&, const PluginConfigView&)>;

    static ScriptPluginRegistry &instance();
    void register_adapter(const std::string &language, FactoryFn factory);
    ScriptPluginAdapter *create(const std::string &language,
                                const PluginManifest &manifest,
                                const PluginConfigView &config) const;
    bool has_adapter(const std::string &language) const;
    std::vector<std::string> available_languages() const;
};
```

各语言模块通过 `register_adapter()` 注册自己的工厂函数，`PluginManager` 通过 `create()` 按 `language` 字段查找对应工厂创建适配器。

## 5. LuaScriptPluginAdapter（Lua 具体实现）

### 5.1 类设计

`LuaScriptPluginAdapter` 继承 `ScriptPluginAdapter`，实现所有 Lua 特定逻辑：

```cpp
class LuaScriptPluginAdapter : public ScriptPluginAdapter
{
public:
    struct Config
    {
        size_t memory_budget_bytes = 8 * 1024 * 1024;
        size_t max_instructions_per_call = 10 * 1000 * 1000;
    };

    explicit LuaScriptPluginAdapter(PluginManifest manifest);
    LuaScriptPluginAdapter(PluginManifest manifest, Config config);
    ~LuaScriptPluginAdapter() override;

    bool load_script(const std::string &script_path) override;

protected:
    bool do_init(const PluginContext &context) override;
    void do_enable() override;
    void do_disable() override;
    void do_release() override;
    bool do_health_check() const override;
    void do_config_changed(const PluginConfigView &config) override;

private:
    static void *lua_budget_allocator(void *ud, void *ptr, size_t osize, size_t nsize);
    static void lua_execution_timeout_hook(lua_State *L, lua_Debug *ar);
    void set_execution_hook();
    void clear_execution_hook();
    bool init_lua_state();
    void apply_sandbox();
    void call_lua_void(const char *func_name);
    bool call_lua_init(const PluginContext &context);
    void call_lua_config_changed(const PluginConfigView &config);

    lua_State *L_ = nullptr;
    Config config_;
    LuaMemoryBudget memory_budget_;
    std::recursive_mutex lua_mutex_;
};
```

### 5.2 模块注册

`init_lua_plugin_module()` 在 `lua_plugin_module.cpp` 中注册 Lua 工厂：

```cpp
void init_lua_plugin_module()
{
    ScriptPluginRegistry::instance().register_adapter("lua",
        [](const PluginManifest &manifest, const PluginConfigView &config) -> ScriptPluginAdapter* {
            LuaScriptPluginAdapter::Config lua_config;
            lua_config.memory_budget_bytes = static_cast<size_t>(
                config.get_int64("lua_memory_budget_bytes", 8 * 1024 * 1024));
            lua_config.max_instructions_per_call = static_cast<size_t>(
                config.get_int64("lua_max_instructions", 10 * 1000 * 1000));
            return new LuaScriptPluginAdapter(manifest, lua_config);
        });
}
```

宿主程序在启动时调用 `init_lua_plugin_module()` 即可启用 Lua 插件支持。

### 5.3 生命周期桥接

| C++ 侧 | Lua 侧 | 必须定义 |
|---|---|---|
| `on_loaded()` | 无（adapter 内部完成 VM 初始化） | 否 |
| `on_init(context)` | `function plugin.on_init(ctx)` | **是** |
| `on_enable()` | `function plugin.on_enable()` | 否 |
| `on_disable()` | `function plugin.on_disable()` | 否 |
| `on_release()` | `function plugin.on_release()` | 否 |
| `on_health_check()` | `function plugin.on_health_check()` | 否（默认 true） |
| `on_config_changed(config)` | `function plugin.on_config_changed(config)` | 否 |

Lua 插件只需定义一个全局 `plugin` 表，`on_init` 是唯一必须的函数：

```lua
local plugin = {}

function plugin.on_init(ctx)
    -- ctx 是只读的上下文表
    ctx.logger:info("hello from Lua plugin!")
    return true
end

function plugin.on_enable()
    -- 可选
end

function plugin.on_release()
    -- 可选
end

return plugin
```

### 5.4 Lua 脚本查找路径

```
plugin_root_path / plugin_name / entry
```

例如 `plugin_root_path = "./plugins/"` 且 `entry = "main.lua"`：
- 查找路径：`./plugins/LuaGreeter/main.lua`
- 同目录下的 `require` 可以使用相对路径

## 6. 宿主能力 Lua 绑定

### 6.1 绑定策略

采用 **userdata + 元表** 方案，将 `Host*` 指针包装为 Lua userdata，通过元表暴露方法。

优势：
- Lua 插件能真正调用宿主能力，不是只读镜像
- 生命周期由 ResourceGuard 管理，安全可控
- 性能好，无 JSON 序列化开销

### 6.2 host.logger

```lua
ctx.logger:trace(message)
ctx.logger:debug(message)
ctx.logger:info(message)
ctx.logger:warn(message)
ctx.logger:error(message)
ctx.logger:fatal(message)
```

实现：将 `HostLogger*` 包为 userdata，元表方法内部调用 `logger->log(level, "", 0, "", message)`。

### 6.3 host.event_bus

```lua
-- 订阅事件（自动资源追踪）
local token = ctx.event_bus:subscribe("plugin.loaded", function(event)
    ctx.logger:info("event received: " .. event.name)
end)

-- 发布事件
ctx.event_bus:publish("my.custom.event", { key = "value" })

-- 取消订阅（通常不需要手动调用，资源守卫自动清理）
ctx.event_bus:unsubscribe(token)
```

实现：
- `subscribe` 时自动通过 `PluginContextHelper::subscribe_event` 注册，ResourceGuard 追踪
- `publish` 将 Lua table 序列化为 `std::any`（轻量包装，不走 JSON）
- Lua 回调以 `std::function` 形式持有，保证回调生命周期

### 6.4 host.scheduler

```lua
-- 一次性延迟任务（自动资源追踪）
ctx.scheduler:schedule_after(3000, function()
    ctx.logger:info("3 seconds elapsed!")
end, "my_plugin.delayed")

-- 间隔任务（自动资源追踪）
ctx.scheduler:schedule_interval(5000, function()
    ctx.logger:info("tick!")
end, "my_plugin.heartbeat")

-- 取消任务
ctx.scheduler:cancel(task_id)
```

实现：
- `schedule_after` / `schedule_interval` 通过 `PluginContextHelper` 调用，自动追踪
- Lua 回调包装为 `std::function<void()>`，Lua 引用通过 registry 持有防 GC
- 延迟单位为毫秒（整数）

### 6.5 host.storage

```lua
-- KV 操作
ctx.storage:set("my_key", "my_value")
local val = ctx.storage:get("my_key")   -- 返回 string 或 nil
ctx.storage:del("my_key")
local exists = ctx.storage:exists("my_key")  -- 返回 bool

-- 带过期
ctx.storage:set("temp_key", "temp_value", 60000)  -- TTL 60秒

-- Hash 操作
ctx.storage:hset("hash_key", "field1", "value1")
local fv = ctx.storage:hget("hash_key", "field1")
ctx.storage:hdel("hash_key", "field1")

-- 可用性检查
if ctx.storage:is_available() then
    -- ...
end
```

实现：将 `HostStorage*` 包为 userdata，每个方法映射为对应 C++ 调用。`std::optional<std::string>` 映射为 Lua string 或 nil。

### 6.6 host.resource_guard

```lua
-- 查询资源状态
local count = ctx.resource_guard:tracked_count()
local has = ctx.resource_guard:has_tracked_resources()
local report = ctx.resource_guard:leak_report()
```

Lua 插件一般不需要手动 `track/untrack`，因为 `event_bus:subscribe` 和 `scheduler:schedule_*` 已自动追踪。`resource_guard` 绑定主要用于查询和调试。

### 6.7 二期绑定（暂不实现）

以下接口因为涉及更复杂的类型系统或 C++ 模板，放到二期：

- `host.service_registry` — 注册/查询服务需要 `std::any` 和模板转型，Lua 侧没有等价机制
- `host.http_interceptor` — 路由回调涉及 `void*` 类型擦除的请求/响应对象
- `host.service_catalog` — 同 service_registry
- `host.permission_guard` — 权限管理是宿主侧职责，插件不应直接操作

## 7. Lua 回调安全

### 7.1 异常隔离

所有 Lua 函数调用统一通过 `lua_pcall` 执行，异常不会穿透到 C++ 侧：

```cpp
bool LuaScriptPluginAdapter::call_lua_void(const char *func_name)
{
    std::lock_guard<std::recursive_mutex> lock(lua_mutex_);
    if (!L_) return;

    lua_getglobal(L_, "plugin_table");
    if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return; }

    lua_getfield(L_, -1, func_name);
    if (!lua_isfunction(L_, -1)) { lua_pop(L_, 2); return; }

    set_execution_hook();
    if (lua_pcall(L_, 0, 0, 0) != LUA_OK) {
        const char *err = lua_tostring(L_, -1);
        LOG_ERROR("lua plugin '{}' {}: {}", manifest_.name, func_name, err ? err : "unknown");
        lua_pop(L_, 1);
    }
    clear_execution_hook();
    lua_pop(L_, 1);
}
```

### 7.2 与 PluginCallGuard 的配合

`LuaScriptPluginAdapter` 的 `on_init` 等方法本身会被 `PluginCallGuard::guarded_call_void` 包裹，形成双重保护：

```
C++ PluginCallGuard (catch C++ exception)
  └─ LuaScriptPluginAdapter::on_init()
       └─ lua_pcall (catch Lua error)
            └─ Lua plugin.on_init(ctx)
```

Lua 侧的错误在 `lua_pcall` 就被捕获，不会抛出 C++ 异常。如果 Lua 回调失败，`on_init` 返回 `false`，CallGuard 正常走故障流程。

### 7.3 回调生命周期

Lua 函数作为 `std::function` 传入 C++ 时，需要防 GC：

```cpp
// 将 Lua 回调存入 registry，返回引用 ID
int ref = luaL_ref(L_, LUA_REGISTRYINDEX);

// C++ 侧持有 ref，需要回调时：
lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
lua_pcall(L_, ...);

// 不再需要时释放：
luaL_unref(L_, LUA_REGISTRYINDEX, ref);
```

所有 Lua 回调引用在 `on_release` 时统一释放，或由 ResourceGuard 的 cleanup 回调释放。

## 8. PluginManager 加载流程变更

### 8.1 load() 方法分支

```cpp
bool PluginManager::load(const std::string &pluginName)
{
    // 1. 尝试查找 plugin.json（脚本插件描述文件）
    std::string json_path = data_->plugin_path_ + pluginName + ".json";
    auto config = load_plugin_config(json_path);

    if (config.loaded()) {
        std::string run_mode_str = config.get_string("run_mode", "native");
        if (run_mode_str == "script") {
            return load_script_plugin(pluginName, config);
        }
    }

    // 2. 没有声明 script → 走原有 native 路径
    return load_native_plugin(pluginName);
}
```

### 8.2 load_script_plugin 流程

```cpp
bool PluginManager::load_script_plugin(const std::string &name, const PluginConfigView &config)
{
    // 1. 从 config 解析 manifest
    PluginManifest manifest;
    manifest.plugin_id = name;
    manifest.name = config.get_string("name", name);
    manifest.version = config.get_string("version", "1.0.0");
    manifest.entry = config.get_string("entry", "main.lua");
    manifest.language = config.get_string("language", "lua");
    manifest.run_mode = PluginRunMode::script;
    // ... 解析 permissions, depends_on 等 ...

    // 2. API 版本校验
    if (manifest.api_version > HOST_API_VERSION) {
        LOG_ERROR("script plugin '{}' requires api_version {}", name, manifest.api_version);
        return false;
    }

    // 3. 通过 ScriptPluginRegistry 创建适配器（语言无关）
    std::string language = manifest.language.empty() ? "lua" : manifest.language;
    auto *adapter = ScriptPluginRegistry::instance().create(language, manifest, config);
    if (!adapter) {
        LOG_ERROR("no script adapter registered for language '{}'", language);
        return false;
    }

    // 4. 加载脚本
    std::string script_path = data_->plugin_path_ + name + "/" + manifest.entry;
    if (!adapter->load_script(script_path)) {
        delete adapter;
        return false;
    }

    // 5. 注册到 plugins_ 表（无 library_handle）
    data_->plugins_[name] = { nullptr, adapter };

    // 6. 注册到 lifecycle_manager
    lifecycle_manager_.register_instance(name, adapter, nullptr);

    // 7. 初始化
    if (!initialize_plugin(name, adapter)) {
        release_plugin(name);
        return false;
    }

    return true;
}
```

## 9. 目录结构

### 9.1 模块化架构

脚本插件系统分为两层：

- **`plugin_core`** — 语言无关的抽象基类 `ScriptPluginAdapter` 和注册机制 `ScriptPluginRegistry`
- **`plugin_lua`** — Lua 特定实现 `LuaScriptPluginAdapter` 及宿主能力绑定

```text
plugins/core/                                  # 语言无关抽象层
  include/plugin/
    script_plugin_adapter.h                    # ScriptPluginAdapter 纯虚基类
    script_plugin_registry.h                   # ScriptPluginRegistry 注册机制
  src/plugin/
    script_plugin_adapter.cpp                  # ScriptPluginAdapter 基类实现
    script_plugin_registry.cpp                 # ScriptPluginRegistry 实现
    plugin_manager.cpp                         # load_script_plugin 通过 registry 创建适配器

plugins/lua/                                   # Lua 特定实现（可选模块）
  include/
    lua_script_plugin_adapter.h                # LuaScriptPluginAdapter + Config + LuaMemoryBudget
    lua_plugin_module.h                        # init_lua_plugin_module() 声明
  src/
    lua_script_plugin_adapter.cpp              # Lua 适配器实现（内存预算/执行超时/沙箱/生命周期桥接）
    lua_host_bindings.h                        # 绑定注册入口声明
    lua_host_bindings.cpp                      # 绑定实现（Logger/EventBus/Scheduler/Storage/ResourceGuard）
    lua_plugin_module.cpp                      # 注册 Lua 工厂到 ScriptPluginRegistry
  CMakeLists.txt                               # 链接 lua_static + PluginCore

plugins/examples/
  lua_greeter/                                 # Lua 示例插件目录
    plugin.json                                # 插件清单
    main.lua                                   # 脚本入口
```

### 9.2 修改文件

```text
plugins/core/include/plugin/plugin_manifest.h   # PluginRunMode::script
plugins/core/include/plugin/plugin_state.h      # can_transition 补 script 相关
plugins/core/src/plugin/plugin_manager.cpp      # load() 分支 + load_script_plugin 用 registry
plugins/core/CMakeLists.txt                     # 移除 lua_static 依赖
plugins/CMakeLists.txt                          # add_subdirectory(lua)
docs/PLUGIN_SYSTEM_DESIGN.md                    # 补充脚本插件章节
```

## 10. Lua 插件 SDK API 参考

### 10.1 插件定义模板

```lua
-- main.lua
-- Lua 插件必须返回一个 plugin 表，on_init 是唯一必须的函数

local plugin = {}

--- 初始化插件（必须实现）
-- @param ctx 上下文对象，包含宿主能力
-- @return boolean 成功返回 true
function plugin.on_init(ctx)
    return true
end

--- 插件启用（可选）
function plugin.on_enable()
end

--- 插件停用（可选）
function plugin.on_disable()
end

--- 插件释放（可选）
function plugin.on_release()
end

--- 健康检查（可选，默认返回 true）
-- @return boolean
function plugin.on_health_check()
    return true
end

--- 配置变更通知（可选）
-- @param config 配置视图
function plugin.on_config_changed(config)
end

return plugin
```

### 10.2 Context 对象

`ctx` 是 `on_init` 的参数，包含以下字段和方法：

```lua
-- 只读元数据
ctx.app_name          -- string
ctx.plugin_name       -- string
ctx.plugin_root_path  -- string
ctx.worker_threads    -- number
ctx.worker_index      -- number
ctx.is_worker_process -- boolean

-- 宿主能力对象（根据权限可用或 nil）
ctx.logger            -- Logger 对象（需 use_logger 权限）
ctx.event_bus         -- EventBus 对象（需 use_event_bus 权限）
ctx.scheduler         -- Scheduler 对象（需 use_scheduler 权限）
ctx.storage           -- Storage 对象（需 use_storage 权限）
ctx.resource_guard    -- ResourceGuard 对象

-- 配置
ctx.config            -- 配置表（从 plugin.json 加载）
```

### 10.3 Logger API

```lua
ctx.logger:trace(message)
ctx.logger:debug(message)
ctx.logger:info(message)
ctx.logger:warn(message)
ctx.logger:error(message)
ctx.logger:fatal(message)
```

### 10.4 EventBus API

```lua
-- 订阅事件（自动资源追踪）
local token = ctx.event_bus:subscribe(event_name, callback)
  -- callback(event): event.name, event.payload (table or nil)

-- 发布事件
ctx.event_bus:publish(event_name, payload_table)

-- 取消订阅
ctx.event_bus:unsubscribe(token)
```

### 10.5 Scheduler API

```lua
-- 一次性延迟任务（毫秒），自动追踪
local task_id = ctx.scheduler:schedule_after(delay_ms, callback, name)

-- 间隔任务（毫秒），自动追踪
local task_id = ctx.scheduler:schedule_interval(interval_ms, callback, name)

-- 取消任务
ctx.scheduler:cancel(task_id)
```

### 10.6 Storage API

```lua
ctx.storage:set(key, value)
ctx.storage:set(key, value, ttl_ms)
local value = ctx.storage:get(key)        -- string | nil
ctx.storage:del(key)
local exists = ctx.storage:exists(key)     -- boolean
ctx.storage:hset(key, field, value)
local value = ctx.storage:hget(key, field) -- string | nil
ctx.storage:hdel(key, field)
local available = ctx.storage:is_available() -- boolean
local backend = ctx.storage:backend_name()   -- string
```

### 10.7 ResourceGuard API

```lua
local count = ctx.resource_guard:tracked_count()       -- number
local has = ctx.resource_guard:has_tracked_resources() -- boolean
local report = ctx.resource_guard:leak_report()        -- string
```

## 11. 示例 Lua 插件：LuaGreeter

```lua
-- plugins/lua_greeter/main.lua

local plugin = {}
local ctx = nil
local tick_count = 0

function plugin.on_init(c)
    ctx = c

    ctx.logger:info("LuaGreeter initializing...")
    ctx.logger:info("app_name = " .. ctx.app_name)
    ctx.logger:info("plugin_name = " .. ctx.plugin_name)

    -- 读取配置
    if ctx.config and ctx.config.greeting then
        ctx.logger:info("greeting from config: " .. ctx.config.greeting)
    end

    -- 订阅事件
    ctx.event_bus:subscribe("plugin.loaded", function(event)
        ctx.logger:info("LuaGreeter saw plugin loaded event")
    end)

    -- 启动心跳定时器
    ctx.scheduler:schedule_interval(10000, function()
        tick_count = tick_count + 1
        ctx.logger:debug("LuaGreeter heartbeat #" .. tick_count)
    end, "LuaGreeter.heartbeat")

    -- 演示存储
    if ctx.storage and ctx.storage:is_available() then
        ctx.storage:set("lua_greeter.status", "running")
        ctx.storage:set("lua_greeter.count", tostring(tick_count), 60000)
        local val = ctx.storage:get("lua_greeter.status")
        ctx.logger:info("storage read back: " .. tostring(val))
    end

    ctx.logger:info("LuaGreeter initialized successfully")
    return true
end

function plugin.on_enable()
    ctx.logger:info("LuaGreeter enabled")
end

function plugin.on_disable()
    ctx.logger:info("LuaGreeter disabled")
end

function plugin.on_health_check()
    return true
end

function plugin.on_release()
    if ctx and ctx.logger then
        ctx.logger:info("LuaGreeter releasing, total ticks = " .. tick_count)
    end
end

return plugin
```

对应的 `plugin.json`：

```json
{
    "plugin_id": "lua_greeter",
    "name": "LuaGreeter",
    "version": "1.0.0",
    "author": "yuan",
    "description": "A demo Lua script plugin",
    "api_version": 1,
    "entry": "main.lua",
    "language": "lua",
    "run_mode": "script",
    "permissions": "use_event_bus,use_logger,use_scheduler,use_storage",
    "depends_on": [],
    "lua_memory_budget_bytes": 8388608,
    "lua_max_instructions": 10000000,
    "greeting": "Hello from Lua!"
}
```

## 12. CMake 变更

### 12.1 core/CMakeLists.txt

```cmake
# Lua 源码构建为静态库（位于 third_party，独立于 plugin_core）
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/lua-5.4.8 ${CMAKE_BINARY_DIR}/lua)

# Lua 适配器作为可选模块
add_subdirectory(plugin_lua)
```

### 12.2 plugins/core/CMakeLists.txt

```cmake
# plugin_core 不依赖 Lua，只含语言无关抽象
target_link_libraries(PluginCore PUBLIC CoreBase Logger dl)
# 不再链接 lua_static，不再包含 Lua 头文件路径
```

### 12.3 plugins/lua/CMakeLists.txt

```cmake
# plugin_lua 是可选模块，依赖 plugin_core + lua_static
add_library(PluginLua ${PLUGIN_LUA_SRC})
target_link_libraries(PluginLua PUBLIC PluginCore lua_static)
target_include_directories(PluginLua PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>"
)
```

### 12.4 启用 Lua 支持

宿主程序需要：
1. 链接 `PluginLua`
2. 在启动时调用 `init_lua_plugin_module()` 注册 Lua 工厂

```cpp
#include "lua_plugin_module.h"

// 在 PluginManager 初始化之前
init_lua_plugin_module();

// 此后 load_script_plugin 会自动通过 registry 找到 Lua 适配器
```

## 13. 安全考量

### 13.1 Lua 沙箱

Lua 5.4 标准库中部分功能对插件系统不安全，应在 `LuaScriptPluginAdapter` 初始化时限制：

| 库 | 处置 | 原因 |
|---|---|---|
| `io` | 整库移除 | 插件不应直接访问文件系统 |
| `os.execute` | 移除 | 防止执行系统命令 |
| `os.exit` | 移除 | 防止退出进程 |
| `os.getenv` | 移除 | 防止读取环境变量 |
| `os.remove` | 移除 | 防止删除文件 |
| `os.rename` | 移除 | 防止重命名文件 |
| `os.tmpname` | 移除 | 防止生成临时文件名 |
| `package.loadlib` | 移除 | 防止加载原生库绕过沙箱 |
| `package.cpath` | 移除 | 防止搜索 C 模块路径 |
| `package.searchers[3]` | 移除 | 阻止 C 模块加载器 |
| `package.searchers[4]` | 移除 | 阻止 all-in-one 加载器 |
| `debug` | 整库移除 | 防止访问内部状态 |
| `string`, `table`, `math`, `utf8` | 保留 | 纯计算，无副作用 |
| `coroutine` | 保留 | Lua 协程是安全的 |

实现方式：在 `luaL_openlibs` 后手动移除不安全库：

```cpp
luaL_openlibs(L_);
// 移除整库
lua_pushnil(L_);
lua_setglobal(L_, "io");
lua_pushnil(L_);
lua_setglobal(L_, "debug");
// 移除 os 危险函数
lua_getglobal(L_, "os");
lua_pushnil(L_); lua_setfield(L_, -2, "execute");
lua_pushnil(L_); lua_setfield(L_, -2, "exit");
lua_pushnil(L_); lua_setfield(L_, -2, "getenv");
lua_pushnil(L_); lua_setfield(L_, -2, "remove");
lua_pushnil(L_); lua_setfield(L_, -2, "rename");
lua_pushnil(L_); lua_setfield(L_, -2, "tmpname");
lua_pop(L_, 1);
// 移除 package 危险项
lua_getglobal(L_, "package");
lua_pushnil(L_); lua_setfield(L_, -2, "loadlib");
lua_pushnil(L_); lua_setfield(L_, -2, "cpath");
lua_getfield(L_, -1, "searchers");
if (lua_istable(L_, -1)) {
    lua_pushnil(L_); lua_seti(L_, -2, 4);  // 移除 all-in-one searcher
    lua_pushnil(L_); lua_seti(L_, -2, 3);  // 移除 C searcher
}
lua_pop(L_, 2);
```

### 13.2 内存限制

通过 `lua_newstate` 传入自定义分配器 `lua_budget_allocator`，控制 Lua 插件的最大内存使用。超出预算时分配器返回 nullptr，Lua 自动触发内存分配失败错误。

```cpp
struct LuaMemoryBudget {
    size_t max_bytes = 8 * 1024 * 1024;  // 默认 8MB
    size_t current_bytes = 0;
};

// 自定义分配器：跟踪 current_bytes，超预算返回 nullptr
static void *lua_budget_allocator(void *ud, void *ptr, size_t osize, size_t nsize);

// 创建受内存限制的 Lua State
L_ = lua_newstate(lua_budget_allocator, &memory_budget_);
```

可通过 `plugin.json` 配置：

```json
{
    "lua_memory_budget_bytes": 4194304
}
```

### 13.3 执行超时

通过 `lua_sethook` + `LUA_MASKCOUNT` 实现指令计数限制，防止 Lua 脚本死循环。所有 `lua_pcall` 调用前后自动设置/清除 hook，包括生命周期回调和 EventBus/Scheduler 的 Lua 回调。

```cpp
// 超时 hook：指令计数超限时调用 luaL_error 终止执行
static void lua_execution_timeout_hook(lua_State *L, lua_Debug *ar);

// LuaScriptPluginAdapter 中：生命周期回调的 hook 保护
void set_execution_hook() const;   // lua_sethook(L_, hook, LUA_MASKCOUNT, max_instructions)
void clear_execution_hook() const; // lua_sethook(L_, nullptr, 0, 0)

// lua_host_bindings.cpp 中：EventBus/Scheduler 回调的 hook 保护
// max_instructions_per_call 存储在 Lua registry 中
void set_callback_hook(lua_State *L);
void clear_callback_hook(lua_State *L);
```

可通过 `plugin.json` 配置：

```json
{
    "lua_max_instructions": 5000000
}
```

LuaScriptPluginAdapter::Config 默认值：

```cpp
struct Config {
    size_t memory_budget_bytes = 8 * 1024 * 1024;       // 8MB
    size_t max_instructions_per_call = 10 * 1000 * 1000; // 1000万条指令
};
```

## 14. 测试策略

### 14.1 单元测试

| 测试项 | 内容 |
|---|---|
| `test_lua_adapter_lifecycle` | adapter 创建、脚本加载、生命周期回调桥接 |
| `test_lua_bindings_logger` | 日志绑定正确调用 HostLogger |
| `test_lua_bindings_event_bus` | 订阅/发布/取消订阅 |
| `test_lua_bindings_scheduler` | 延迟任务/间隔任务/取消 |
| `test_lua_bindings_storage` | KV/Hash 操作 |
| `test_lua_error_handling` | Lua 脚本错误不穿透 C++，fault 计数正确 |
| `test_lua_sandbox` | 不安全库不可访问 |
| `test_lua_resource_tracking` | 事件订阅和定时任务自动追踪 |
| `test_lua_script_missing_on_init` | 缺少 on_init 时 load_script 失败 |
| `test_lua_script_not_returning_table` | 脚本不返回 table 时 load_script 失败 |
| `test_lua_adapter_config_changed` | on_config_changed 回调正确传递配置 |
| `test_lua_on_init_returns_false` | on_init 返回 false 正确传播 |
| `test_lua_on_disable_error` | on_disable 中 Lua 错误被安全捕获 |
| `test_lua_health_check_returns_false` | on_health_check 返回 false 正确传播 |
| `test_lua_event_bus_publish_with_payload` | EventBus 发布带 payload 的事件 |
| `test_lua_memory_budget` | 内存预算超限时 on_init 失败 |
| `test_lua_execution_timeout` | 指令计数超限时 on_init 失败 |
| `test_lua_sandbox_no_c_searcher` | package.searchers 只剩 2 个（preload + lua） |
| `test_lua_callback_timeout_eventbus` | EventBus 回调中死循环被指令计数 hook 终止 |
| `test_lua_callback_timeout_scheduler` | Scheduler 回调中死循环被指令计数 hook 终止 |

### 14.2 集成测试

通过 `PluginManager::load("LuaGreeter")` 端到端验证 Lua 插件加载、初始化、激活、释放全流程。

## 15. 演进路线

### 一期（已完成）

- `PluginRunMode::script` + `ScriptPluginAdapter`（纯虚基类）+ `LuaScriptPluginAdapter`
- `ScriptPluginRegistry` 语言注册机制
- 绑定：Logger / EventBus / Scheduler / Storage / ResourceGuard
- Lua 沙箱（移除不安全库 + C searcher 移除）
- Lua 内存预算（`lua_newstate` + 自定义分配器）
- 执行超时（`lua_sethook` + 指令计数，覆盖生命周期回调和 EventBus/Scheduler 回调）
- 示例插件 LuaGreeter
- 20 个单元测试

### 二期（待实现）

- 绑定：ServiceRegistry / ServiceCatalog / HttpInterceptor
- 更多脚本语言支持（language 字段区分，如 "javascript"）
- 热重载（监控脚本文件变更，自动 reload）

## 16. 与现有系统的兼容性

| 维度 | 影响 |
|---|---|
| C++ 原生插件 | 零影响，load() 根据 run_mode 分流 |
| PluginLifecycleManager | 零影响，adapter 是普通 Plugin 子类 |
| PluginCallGuard | 零影响，adapter 的 on_init 等仍被 guard 包裹 |
| HostResourceGuard | 零影响，Lua 回调通过 C++ wrapper 注册追踪 |
| PluginHostService | 零影响，load_plugin/unload_plugin 不感知内部是 C++ 还是 Lua |
| PluginSandboxDelegate | 不冲突，sandbox 是进程外隔离，Lua 是进程内语言级安全 |
