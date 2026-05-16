#include "lua_script_plugin_adapter.h"
#include "plugin/plugin_context.h"
#include "plugin/plugin_manifest.h"
#include "plugin/host_logger.h"
#include "plugin/host_event_bus.h"
#include "plugin/host_scheduler.h"
#include "plugin/host_storage.h"
#include "plugin/host_resource_guard.h"
#include "plugin_resource_guard.h"
#include "nlohmann/json.hpp"

#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{

    void require(bool condition, const std::string &message)
    {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            std::exit(1);
        }
    }

    class MockLogger : public yuan::plugin::HostLogger
    {
    public:
        void log(yuan::plugin::HostLogLevel level,
                 const char *, int, const char *,
                 std::string_view message) override
        {
            std::lock_guard<std::mutex> lock(mutex_);
            messages_.push_back({ level, std::string(message) });
        }

        struct Entry
        {
            yuan::plugin::HostLogLevel level;
            std::string message;
        };

        std::vector<Entry> messages_;
        std::mutex mutex_;
    };

    class MockEventBus : public yuan::plugin::HostEventBus
    {
    public:
        yuan::plugin::HostEventSubscription subscribe(const std::string &event_name,
                                                      yuan::plugin::HostEventHandler handler) override
        {
            auto token = next_token_++;
            handlers_[event_name].push_back({ token, std::move(handler) });
            return token;
        }

        bool unsubscribe(yuan::plugin::HostEventSubscription token) override
        {
            for (auto & [
                            name,
                            list
                        ] : handlers_) {
                for (auto it = list.begin(); it != list.end(); ++it) {
                    if (it->token == token) {
                        list.erase(it);
                        return true;
                    }
                }
            }
            return false;
        }

        void publish(std::string event_name, std::any payload = {}) override
        {
            auto it = handlers_.find(event_name);
            if (it != handlers_.end()) {
                yuan::plugin::HostEvent event;
                event.name = event_name;
                event.payload = std::move(payload);
                for (auto &entry : it->second) {
                    entry.handler(event);
                }
            }
        }

        struct HandlerEntry
        {
            yuan::plugin::HostEventSubscription token;
            yuan::plugin::HostEventHandler handler;
        };

        std::unordered_map<std::string, std::vector<HandlerEntry> > handlers_;
        yuan::plugin::HostEventSubscription next_token_ = 1;
    };

    class MockScheduler : public yuan::plugin::HostScheduler
    {
    public:
        yuan::plugin::HostSchedulerTaskId schedule_after(std::chrono::milliseconds,
                                                         yuan::plugin::HostSchedulerCallback callback,
                                                         const std::string &name) override
        {
            auto id = next_id_++;
            after_callbacks_.push_back({ id, std::move(callback), name });
            return id;
        }

        yuan::plugin::HostSchedulerTaskId schedule_interval(std::chrono::milliseconds,
                                                            yuan::plugin::HostSchedulerCallback callback,
                                                            const std::string &name) override
        {
            auto id = next_id_++;
            interval_callbacks_.push_back({ id, std::move(callback), name });
            return id;
        }

        bool cancel(yuan::plugin::HostSchedulerTaskId id) override
        {
            return true;
        }

        void cancel_by_prefix(const std::string &) override
        {
        }

        bool is_running() const override
        {
            return true;
        }

        struct TaskEntry
        {
            yuan::plugin::HostSchedulerTaskId id;
            yuan::plugin::HostSchedulerCallback callback;
            std::string name;
        };

        std::vector<TaskEntry> after_callbacks_;
        std::vector<TaskEntry> interval_callbacks_;
        yuan::plugin::HostSchedulerTaskId next_id_ = 1;
    };

    class MockStorage : public yuan::plugin::HostStorage
    {
    public:
        bool set(const std::string &key, const std::string &value) override
        {
            kv_[key] = value;
            return true;
        }

        bool set(const std::string &key, const std::string &value,
                 std::chrono::milliseconds) override
        {
            kv_[key] = value;
            return true;
        }

        std::optional<std::string> get(const std::string &key) const override
        {
            auto it = kv_.find(key);
            if (it != kv_.end()) {
                return it->second;
            }
            return std::nullopt;
        }

        bool del(const std::string &key) override
        {
            kv_.erase(key);
            return true;
        }

        bool exists(const std::string &key) const override
        {
            return kv_.count(key) > 0;
        }

        bool hset(const std::string &key, const std::string &field, const std::string &value) override
        {
            hash_[key][field] = value;
            return true;
        }

        std::optional<std::string> hget(const std::string &key, const std::string &field) const override
        {
            auto it = hash_.find(key);
            if (it != hash_.end()) {
                auto fit = it->second.find(field);
                if (fit != it->second.end()) {
                    return fit->second;
                }
            }
            return std::nullopt;
        }

        bool hdel(const std::string &key, const std::string &field) override
        {
            auto it = hash_.find(key);
            if (it != hash_.end()) {
                it->second.erase(field);
            }
            return true;
        }

        std::unordered_map<std::string, std::string> hgetall(const std::string &key) const override
        {
            auto it = hash_.find(key);
            if (it != hash_.end()) {
                return it->second;
            }
            return {};
        }

        bool is_available() const override
        {
            return available_;
        }

        const char *backend_name() const override
        {
            return "mock";
        }

        std::unordered_map<std::string, std::string> kv_;
        std::unordered_map<std::string, std::unordered_map<std::string, std::string> > hash_;
        bool available_ = true;
    };

    std::string write_temp_script(const std::string &dir, const std::string &filename, const std::string &content)
    {
        std::filesystem::create_directories(dir);
        std::string path = dir + "/" + filename;
        std::ofstream ofs(path);
        ofs << content;
        ofs.close();
        return path;
    }

    void remove_temp_dir(const std::string &dir)
    {
        std::filesystem::remove_all(dir);
    }

    yuan::plugin::PluginContext make_test_context(MockLogger &logger,
                                                  MockEventBus &bus,
                                                  MockScheduler &sched,
                                                  MockStorage &storage,
                                                  yuan::app::PluginResourceGuard &guard)
    {
        yuan::plugin::PluginContext ctx;
        ctx.app_name = "test";
        ctx.plugin_name = "lua_test";
        ctx.plugin_root_path = "./";
        ctx.logger = &logger;
        ctx.event_bus = &bus;
        ctx.scheduler = &sched;
        ctx.storage = &storage;
        ctx.resource_guard = &guard;
        ctx.granted_permissions = yuan::plugin::PluginPermission::use_logger |
                                  yuan::plugin::PluginPermission::use_event_bus |
                                  yuan::plugin::PluginPermission::use_scheduler |
                                  yuan::plugin::PluginPermission::use_storage;
        return ctx;
    }

    // ---- test_lua_adapter_lifecycle ----

    void test_lua_adapter_lifecycle()
    {
        std::string tmp = "/tmp/lua_test_lifecycle";
        std::string script_content =
            "local plugin = {}\n"
            "local inited = false\n"
            "local enabled = false\n"
            "local disabled = false\n"
            "function plugin.on_init(ctx)\n"
            "    inited = true\n"
            "    return true\n"
            "end\n"
            "function plugin.on_enable()\n"
            "    enabled = true\n"
            "end\n"
            "function plugin.on_disable()\n"
            "    disabled = true\n"
            "end\n"
            "function plugin.on_release()\n"
            "end\n"
            "function plugin.on_health_check() return true end\n"
            "return plugin\n";
        write_temp_script(tmp, "main.lua", script_content);

        yuan::plugin::PluginManifest manifest;
        manifest.name = "lua_test";
        manifest.entry = "main.lua";
        manifest.run_mode = yuan::plugin::PluginRunMode::script;

        yuan::plugin::LuaScriptPluginAdapter adapter(manifest);
        require(adapter.load_script(tmp + "/main.lua"), "load_script should succeed");

        MockLogger logger;
        MockEventBus bus;
        MockScheduler sched;
        MockStorage storage;
        yuan::app::PluginResourceGuard guard;
        auto ctx = make_test_context(logger, bus, sched, storage, guard);

        require(adapter.on_init(ctx), "on_init should return true");
        adapter.on_enable();
        adapter.on_disable();
        require(adapter.on_health_check(), "on_health_check should return true");

        auto meta = adapter.meta();
        require(meta.name == "lua_test", "meta name should match");

        auto m = adapter.manifest();
        require(m.run_mode == yuan::plugin::PluginRunMode::script, "manifest run_mode should be script");

        adapter.on_release();
        remove_temp_dir(tmp);
    }

    // ---- test_lua_bindings_logger ----

    void test_lua_bindings_logger()
    {
        std::string tmp = "/tmp/lua_test_logger";
        std::string script_content =
            "local plugin = {}\n"
            "function plugin.on_init(ctx)\n"
            "    ctx.logger:info('hello from lua')\n"
            "    ctx.logger:warn('warning msg')\n"
            "    ctx.logger:error('error msg')\n"
            "    return true\n"
            "end\n"
            "function plugin.on_release() end\n"
            "return plugin\n";
        write_temp_script(tmp, "main.lua", script_content);

        yuan::plugin::PluginManifest manifest;
        manifest.name = "lua_test";
        manifest.entry = "main.lua";
        manifest.run_mode = yuan::plugin::PluginRunMode::script;

        yuan::plugin::LuaScriptPluginAdapter adapter(manifest);
        require(adapter.load_script(tmp + "/main.lua"), "load_script should succeed");

        MockLogger logger;
        MockEventBus bus;
        MockScheduler sched;
        MockStorage storage;
        yuan::app::PluginResourceGuard guard;
        auto ctx = make_test_context(logger, bus, sched, storage, guard);

        require(adapter.on_init(ctx), "on_init should succeed");

        require(logger.messages_.size() >= 3, "logger should have received at least 3 messages");

        bool found_info = false, found_warn = false, found_error = false;
        for (auto &m : logger.messages_) {
            if (m.level == yuan::plugin::HostLogLevel::info && m.message.find("hello from lua") != std::string::npos) {
                found_info = true;
            }
            if (m.level == yuan::plugin::HostLogLevel::warn && m.message.find("warning msg") != std::string::npos) {
                found_warn = true;
            }
            if (m.level == yuan::plugin::HostLogLevel::error && m.message.find("error msg") != std::string::npos) {
                found_error = true;
            }
        }
        require(found_info, "should find info log message");
        require(found_warn, "should find warn log message");
        require(found_error, "should find error log message");

        adapter.on_release();
        remove_temp_dir(tmp);
    }

    // ---- test_lua_bindings_event_bus ----

    void test_lua_bindings_event_bus()
    {
        std::string tmp = "/tmp/lua_test_eventbus";
        std::string script_content =
            "local plugin = {}\n"
            "function plugin.on_init(ctx)\n"
            "    ctx.event_bus:subscribe('test.event', function(event)\n"
            "        ctx.logger:info('got event: '..event.name)\n"
            "    end)\n"
            "    ctx.event_bus:publish('test.event')\n"
            "    return true\n"
            "end\n"
            "function plugin.on_release() end\n"
            "return plugin\n";
        write_temp_script(tmp, "main.lua", script_content);

        yuan::plugin::PluginManifest manifest;
        manifest.name = "lua_test";
        manifest.entry = "main.lua";
        manifest.run_mode = yuan::plugin::PluginRunMode::script;

        yuan::plugin::LuaScriptPluginAdapter adapter(manifest);
        require(adapter.load_script(tmp + "/main.lua"), "load_script should succeed");

        MockLogger logger;
        MockEventBus bus;
        MockScheduler sched;
        MockStorage storage;
        yuan::app::PluginResourceGuard guard;
        auto ctx = make_test_context(logger, bus, sched, storage, guard);

        require(adapter.on_init(ctx), "on_init should succeed");

        bool found_event_log = false;
        for (auto &m : logger.messages_) {
            if (m.message.find("got event: test.event") != std::string::npos) {
                found_event_log = true;
            }
        }
        require(found_event_log, "should find event callback log message");

        adapter.on_release();
        remove_temp_dir(tmp);
    }

    // ---- test_lua_bindings_scheduler ----

    void test_lua_bindings_scheduler()
    {
        std::string tmp = "/tmp/lua_test_scheduler";
        std::string script_content =
            "local plugin = {}\n"
            "function plugin.on_init(ctx)\n"
            "    local id1 = ctx.scheduler:schedule_after(1000, function()\n"
            "        ctx.logger:info('after fired')\n"
            "    end, 'test.after')\n"
            "    local id2 = ctx.scheduler:schedule_interval(5000, function()\n"
            "        ctx.logger:info('interval fired')\n"
            "    end, 'test.interval')\n"
            "    ctx.logger:info('scheduled: '..tostring(id1)..', '..tostring(id2))\n"
            "    return true\n"
            "end\n"
            "function plugin.on_release() end\n"
            "return plugin\n";
        write_temp_script(tmp, "main.lua", script_content);

        yuan::plugin::PluginManifest manifest;
        manifest.name = "lua_test";
        manifest.entry = "main.lua";
        manifest.run_mode = yuan::plugin::PluginRunMode::script;

        yuan::plugin::LuaScriptPluginAdapter adapter(manifest);
        require(adapter.load_script(tmp + "/main.lua"), "load_script should succeed");

        MockLogger logger;
        MockEventBus bus;
        MockScheduler sched;
        MockStorage storage;
        yuan::app::PluginResourceGuard guard;
        auto ctx = make_test_context(logger, bus, sched, storage, guard);

        require(adapter.on_init(ctx), "on_init should succeed");

        require(sched.after_callbacks_.size() >= 1, "should have at least 1 schedule_after callback");
        require(sched.interval_callbacks_.size() >= 1, "should have at least 1 schedule_interval callback");

        sched.after_callbacks_[0].callback();
        bool found_after = false;
        for (auto &m : logger.messages_) {
            if (m.message.find("after fired") != std::string::npos) {
                found_after = true;
            }
        }
        require(found_after, "schedule_after callback should produce log");

        adapter.on_release();
        remove_temp_dir(tmp);
    }

    // ---- test_lua_bindings_storage ----

    void test_lua_bindings_storage()
    {
        std::string tmp = "/tmp/lua_test_storage";
        std::string script_content =
            "local plugin = {}\n"
            "function plugin.on_init(ctx)\n"
            "    ctx.storage:set('k1', 'v1')\n"
            "    local val = ctx.storage:get('k1')\n"
            "    ctx.logger:info('get k1 = '..tostring(val))\n"
            "    ctx.storage:hset('hk', 'f1', 'fv1')\n"
            "    local fv = ctx.storage:hget('hk', 'f1')\n"
            "    ctx.logger:info('hget hk.f1 = '..tostring(fv))\n"
            "    local avail = ctx.storage:is_available()\n"
            "    ctx.logger:info('available = '..tostring(avail))\n"
            "    ctx.storage:del('k1')\n"
            "    local exists = ctx.storage:exists('k1')\n"
            "    ctx.logger:info('k1 exists after del = '..tostring(exists))\n"
            "    return true\n"
            "end\n"
            "function plugin.on_release() end\n"
            "return plugin\n";
        write_temp_script(tmp, "main.lua", script_content);

        yuan::plugin::PluginManifest manifest;
        manifest.name = "lua_test";
        manifest.entry = "main.lua";
        manifest.run_mode = yuan::plugin::PluginRunMode::script;

        yuan::plugin::LuaScriptPluginAdapter adapter(manifest);
        require(adapter.load_script(tmp + "/main.lua"), "load_script should succeed");

        MockLogger logger;
        MockEventBus bus;
        MockScheduler sched;
        MockStorage storage;
        yuan::app::PluginResourceGuard guard;
        auto ctx = make_test_context(logger, bus, sched, storage, guard);

        require(adapter.on_init(ctx), "on_init should succeed");

        require(storage.kv_.count("k1") == 0, "k1 should be deleted");
        require(storage.hash_.count("hk") > 0, "hk should exist in hash");
        require(storage.hash_["hk"]["f1"] == "fv1", "hk.f1 should be fv1");

        bool found_get = false;
        for (auto &m : logger.messages_) {
            if (m.message.find("get k1 = v1") != std::string::npos) {
                found_get = true;
            }
        }
        require(found_get, "should find get result log");

        adapter.on_release();
        remove_temp_dir(tmp);
    }

    // ---- test_lua_error_handling ----

    void test_lua_error_handling()
    {
        std::string tmp = "/tmp/lua_test_error";
        std::string script_content =
            "local plugin = {}\n"
            "function plugin.on_init(ctx) return true end\n"
            "function plugin.on_enable()\n"
            "    error('intentional error from lua')\n"
            "end\n"
            "function plugin.on_release() end\n"
            "return plugin\n";
        write_temp_script(tmp, "main.lua", script_content);

        yuan::plugin::PluginManifest manifest;
        manifest.name = "lua_test";
        manifest.entry = "main.lua";
        manifest.run_mode = yuan::plugin::PluginRunMode::script;

        yuan::plugin::LuaScriptPluginAdapter adapter(manifest);
        require(adapter.load_script(tmp + "/main.lua"), "load_script should succeed");

        MockLogger logger;
        MockEventBus bus;
        MockScheduler sched;
        MockStorage storage;
        yuan::app::PluginResourceGuard guard;
        auto ctx = make_test_context(logger, bus, sched, storage, guard);

        require(adapter.on_init(ctx), "on_init should succeed");

        adapter.on_enable();

        bool found_error = false;
        for (auto &m : logger.messages_) {
            if (m.message.find("intentional error") != std::string::npos) {
                found_error = true;
            }
        }
        require(found_error, "should find lua error in host logger");

        adapter.on_release();
        remove_temp_dir(tmp);
    }

    // ---- test_lua_sandbox ----

    void test_lua_sandbox()
    {
        std::string tmp = "/tmp/lua_test_sandbox";
        std::string script_content =
            "local plugin = {}\n"
            "function plugin.on_init(ctx)\n"
            "    local io_exists = (io ~= nil)\n"
            "    ctx.logger:info('io exists = '..tostring(io_exists))\n"
            "    local debug_exists = (debug ~= nil)\n"
            "    ctx.logger:info('debug exists = '..tostring(debug_exists))\n"
            "    local os_execute = nil\n"
            "    if os then os_execute = os.execute end\n"
            "    ctx.logger:info('os.execute exists = '..tostring(os_execute ~= nil))\n"
            "    local loadlib = nil\n"
            "    if package then loadlib = package.loadlib end\n"
            "    ctx.logger:info('package.loadlib exists = '..tostring(loadlib ~= nil))\n"
            "    local os_remove = nil\n"
            "    if os then os_remove = os.remove end\n"
            "    ctx.logger:info('os.remove exists = '..tostring(os_remove ~= nil))\n"
            "    return true\n"
            "end\n"
            "function plugin.on_release() end\n"
            "return plugin\n";
        write_temp_script(tmp, "main.lua", script_content);

        yuan::plugin::PluginManifest manifest;
        manifest.name = "lua_test";
        manifest.entry = "main.lua";
        manifest.run_mode = yuan::plugin::PluginRunMode::script;

        yuan::plugin::LuaScriptPluginAdapter adapter(manifest);
        require(adapter.load_script(tmp + "/main.lua"), "load_script should succeed");

        MockLogger logger;
        MockEventBus bus;
        MockScheduler sched;
        MockStorage storage;
        yuan::app::PluginResourceGuard guard;
        auto ctx = make_test_context(logger, bus, sched, storage, guard);

        require(adapter.on_init(ctx), "on_init should succeed");

        bool io_nil = false, debug_nil = false, os_execute_nil = false, loadlib_nil = false, os_remove_nil = false;
        for (auto &m : logger.messages_) {
            if (m.message.find("io exists = false") != std::string::npos)
                io_nil = true;
            if (m.message.find("debug exists = false") != std::string::npos)
                debug_nil = true;
            if (m.message.find("os.execute exists = false") != std::string::npos)
                os_execute_nil = true;
            if (m.message.find("package.loadlib exists = false") != std::string::npos)
                loadlib_nil = true;
            if (m.message.find("os.remove exists = false") != std::string::npos)
                os_remove_nil = true;
        }
        require(io_nil, "io library should be removed in sandbox");
        require(debug_nil, "debug library should be removed in sandbox");
        require(os_execute_nil, "os.execute should be removed in sandbox");
        require(loadlib_nil, "package.loadlib should be removed in sandbox");
        require(os_remove_nil, "os.remove should be removed in sandbox");

        adapter.on_release();
        remove_temp_dir(tmp);
    }

    // ---- test_lua_resource_tracking ----

    void test_lua_resource_tracking()
    {
        std::string tmp = "/tmp/lua_test_tracking";
        std::string script_content =
            "local plugin = {}\n"
            "function plugin.on_init(ctx)\n"
            "    ctx.event_bus:subscribe('tracked.event', function(event) end)\n"
            "    ctx.scheduler:schedule_after(1000, function() end, 'tracked.after')\n"
            "    return true\n"
            "end\n"
            "function plugin.on_release() end\n"
            "return plugin\n";
        write_temp_script(tmp, "main.lua", script_content);

        yuan::plugin::PluginManifest manifest;
        manifest.name = "lua_test";
        manifest.entry = "main.lua";
        manifest.run_mode = yuan::plugin::PluginRunMode::script;

        yuan::plugin::LuaScriptPluginAdapter adapter(manifest);
        require(adapter.load_script(tmp + "/main.lua"), "load_script should succeed");

        MockLogger logger;
        MockEventBus bus;
        MockScheduler sched;
        MockStorage storage;
        yuan::app::PluginResourceGuard guard;
        auto ctx = make_test_context(logger, bus, sched, storage, guard);

        require(adapter.on_init(ctx), "on_init should succeed");

        require(guard.tracked_count("lua_test") >= 2,
                "should have at least 2 tracked resources (event + scheduler)");

        adapter.on_release();
        remove_temp_dir(tmp);
    }

    // ---- test_lua_script_missing_on_init ----

    void test_lua_script_missing_on_init()
    {
        std::string tmp = "/tmp/lua_test_no_oninit";
        std::string script_content =
            "local plugin = {}\n"
            "function plugin.on_release() end\n"
            "return plugin\n";
        write_temp_script(tmp, "main.lua", script_content);

        yuan::plugin::PluginManifest manifest;
        manifest.name = "lua_test";
        manifest.entry = "main.lua";
        manifest.run_mode = yuan::plugin::PluginRunMode::script;

        yuan::plugin::LuaScriptPluginAdapter adapter(manifest);
        require(!adapter.load_script(tmp + "/main.lua"),
                "load_script should fail when on_init is missing");

        remove_temp_dir(tmp);
    }

    // ---- test_lua_script_not_returning_table ----

    void test_lua_script_not_returning_table()
    {
        std::string tmp = "/tmp/lua_test_noreturn";
        std::string script_content = "return 42\n";
        write_temp_script(tmp, "main.lua", script_content);

        yuan::plugin::PluginManifest manifest;
        manifest.name = "lua_test";
        manifest.entry = "main.lua";
        manifest.run_mode = yuan::plugin::PluginRunMode::script;

        yuan::plugin::LuaScriptPluginAdapter adapter(manifest);
        require(!adapter.load_script(tmp + "/main.lua"),
                "load_script should fail when script doesn't return a table");

        remove_temp_dir(tmp);
    }

    // ---- test_lua_adapter_config_changed ----

    void test_lua_adapter_config_changed()
    {
        std::string tmp = "/tmp/lua_test_config";

        std::string script_content =
            "local plugin = {}\n"
            "local saved_ctx = nil\n"
            "function plugin.on_init(ctx)\n"
            "    saved_ctx = ctx\n"
            "    return true\n"
            "end\n"
            "function plugin.on_config_changed(config)\n"
            "    if saved_ctx and saved_ctx.logger and config and config.key then\n"
            "        saved_ctx.logger:info('config key = ' .. config.key)\n"
            "    end\n"
            "end\n"
            "function plugin.on_release() end\n"
            "return plugin\n";
        write_temp_script(tmp, "main.lua", script_content);

        yuan::plugin::PluginManifest manifest;
        manifest.name = "lua_test";
        manifest.entry = "main.lua";
        manifest.run_mode = yuan::plugin::PluginRunMode::script;

        yuan::plugin::LuaScriptPluginAdapter adapter(manifest);
        require(adapter.load_script(tmp + "/main.lua"), "load_script should succeed");

        MockLogger logger;
        MockEventBus bus;
        MockScheduler sched;
        MockStorage storage;
        yuan::app::PluginResourceGuard guard;
        auto ctx = make_test_context(logger, bus, sched, storage, guard);

        require(adapter.on_init(ctx), "on_init should succeed");

        auto config_json = std::make_shared<nlohmann::json>();
        (*config_json)["key"] = "value";
        yuan::plugin::PluginConfigView config(std::move(config_json));

        adapter.on_config_changed(config);

        adapter.on_release();
        remove_temp_dir(tmp);
    }

    // ---- test_lua_on_init_returns_false ----

    void test_lua_on_init_returns_false()
    {
        std::string tmp = "/tmp/lua_test_init_false";
        std::string script_content =
            "local plugin = {}\n"
            "function plugin.on_init(ctx)\n"
            "    return false\n"
            "end\n"
            "function plugin.on_release() end\n"
            "return plugin\n";
        write_temp_script(tmp, "main.lua", script_content);

        yuan::plugin::PluginManifest manifest;
        manifest.name = "lua_test";
        manifest.entry = "main.lua";
        manifest.run_mode = yuan::plugin::PluginRunMode::script;

        yuan::plugin::LuaScriptPluginAdapter adapter(manifest);
        require(adapter.load_script(tmp + "/main.lua"), "load_script should succeed");

        MockLogger logger;
        MockEventBus bus;
        MockScheduler sched;
        MockStorage storage;
        yuan::app::PluginResourceGuard guard;
        auto ctx = make_test_context(logger, bus, sched, storage, guard);

        require(!adapter.on_init(ctx), "on_init returning false should propagate");

        adapter.on_release();
        remove_temp_dir(tmp);
    }

    // ---- test_lua_on_disable_error ----

    void test_lua_on_disable_error()
    {
        std::string tmp = "/tmp/lua_test_disable_err";
        std::string script_content =
            "local plugin = {}\n"
            "function plugin.on_init(ctx) return true end\n"
            "function plugin.on_disable()\n"
            "    error('disable error')\n"
            "end\n"
            "function plugin.on_release() end\n"
            "return plugin\n";
        write_temp_script(tmp, "main.lua", script_content);

        yuan::plugin::PluginManifest manifest;
        manifest.name = "lua_test";
        manifest.entry = "main.lua";
        manifest.run_mode = yuan::plugin::PluginRunMode::script;

        yuan::plugin::LuaScriptPluginAdapter adapter(manifest);
        require(adapter.load_script(tmp + "/main.lua"), "load_script should succeed");

        MockLogger logger;
        MockEventBus bus;
        MockScheduler sched;
        MockStorage storage;
        yuan::app::PluginResourceGuard guard;
        auto ctx = make_test_context(logger, bus, sched, storage, guard);

        require(adapter.on_init(ctx), "on_init should succeed");
        adapter.on_disable();

        bool found_error = false;
        for (auto &m : logger.messages_) {
            if (m.message.find("disable error") != std::string::npos) {
                found_error = true;
            }
        }
        require(found_error, "should find on_disable lua error in host logger");

        adapter.on_release();
        remove_temp_dir(tmp);
    }

    // ---- test_lua_health_check_returns_false ----

    void test_lua_health_check_returns_false()
    {
        std::string tmp = "/tmp/lua_test_hc_false";
        std::string script_content =
            "local plugin = {}\n"
            "function plugin.on_init(ctx) return true end\n"
            "function plugin.on_health_check() return false end\n"
            "function plugin.on_release() end\n"
            "return plugin\n";
        write_temp_script(tmp, "main.lua", script_content);

        yuan::plugin::PluginManifest manifest;
        manifest.name = "lua_test";
        manifest.entry = "main.lua";
        manifest.run_mode = yuan::plugin::PluginRunMode::script;

        yuan::plugin::LuaScriptPluginAdapter adapter(manifest);
        require(adapter.load_script(tmp + "/main.lua"), "load_script should succeed");

        MockLogger logger;
        MockEventBus bus;
        MockScheduler sched;
        MockStorage storage;
        yuan::app::PluginResourceGuard guard;
        auto ctx = make_test_context(logger, bus, sched, storage, guard);

        require(adapter.on_init(ctx), "on_init should succeed");
        require(!adapter.on_health_check(), "on_health_check returning false should propagate");

        adapter.on_release();
        remove_temp_dir(tmp);
    }

    // ---- test_lua_event_bus_publish_with_payload ----

    void test_lua_event_bus_publish_with_payload()
    {
        std::string tmp = "/tmp/lua_test_eventbus_payload";
        std::string script_content =
            "local plugin = {}\n"
            "function plugin.on_init(ctx)\n"
            "    ctx.event_bus:subscribe('payload.event', function(event)\n"
            "        if event.payload and event.payload.key then\n"
            "            ctx.logger:info('payload key = ' .. event.payload.key)\n"
            "        end\n"
            "    end)\n"
            "    ctx.event_bus:publish('payload.event', { key = 'value' })\n"
            "    return true\n"
            "end\n"
            "function plugin.on_release() end\n"
            "return plugin\n";
        write_temp_script(tmp, "main.lua", script_content);

        yuan::plugin::PluginManifest manifest;
        manifest.name = "lua_test";
        manifest.entry = "main.lua";
        manifest.run_mode = yuan::plugin::PluginRunMode::script;

        yuan::plugin::LuaScriptPluginAdapter adapter(manifest);
        require(adapter.load_script(tmp + "/main.lua"), "load_script should succeed");

        MockLogger logger;
        MockEventBus bus;
        MockScheduler sched;
        MockStorage storage;
        yuan::app::PluginResourceGuard guard;
        auto ctx = make_test_context(logger, bus, sched, storage, guard);

        require(adapter.on_init(ctx), "on_init should succeed");

        bool found_payload_log = false;
        for (auto &m : logger.messages_) {
            if (m.message.find("payload key = value") != std::string::npos) {
                found_payload_log = true;
            }
        }
        require(found_payload_log, "should find payload data in event callback log");

        adapter.on_release();
        remove_temp_dir(tmp);
    }

    // ---- test_lua_memory_budget ----

    void test_lua_memory_budget()
    {
        std::string tmp = "/tmp/lua_test_membudget";
        std::string script_content =
            "local plugin = {}\n"
            "function plugin.on_init(ctx)\n"
            "    local t = {}\n"
            "    for i = 1, 100000 do\n"
            "        t[i] = string.rep('x', 100)\n"
            "    end\n"
            "    return true\n"
            "end\n"
            "function plugin.on_release() end\n"
            "return plugin\n";
        write_temp_script(tmp, "main.lua", script_content);

        yuan::plugin::PluginManifest manifest;
        manifest.name = "lua_test";
        manifest.entry = "main.lua";
        manifest.run_mode = yuan::plugin::PluginRunMode::script;

        yuan::plugin::LuaScriptPluginAdapter::Config lua_config;
        lua_config.memory_budget_bytes = 512 * 1024;

        yuan::plugin::LuaScriptPluginAdapter adapter(manifest, lua_config);
        require(adapter.load_script(tmp + "/main.lua"), "load_script should succeed");

        MockLogger logger;
        MockEventBus bus;
        MockScheduler sched;
        MockStorage storage;
        yuan::app::PluginResourceGuard guard;
        auto ctx = make_test_context(logger, bus, sched, storage, guard);

        bool init_result = adapter.on_init(ctx);
        require(!init_result, "on_init should fail when memory budget exceeded");

        adapter.on_release();
        remove_temp_dir(tmp);
    }

    // ---- test_lua_execution_timeout ----

    void test_lua_execution_timeout()
    {
        std::string tmp = "/tmp/lua_test_timeout";
        std::string script_content =
            "local plugin = {}\n"
            "function plugin.on_init(ctx)\n"
            "    local i = 0\n"
            "    while true do\n"
            "        i = i + 1\n"
            "    end\n"
            "    return true\n"
            "end\n"
            "function plugin.on_release() end\n"
            "return plugin\n";
        write_temp_script(tmp, "main.lua", script_content);

        yuan::plugin::PluginManifest manifest;
        manifest.name = "lua_test";
        manifest.entry = "main.lua";
        manifest.run_mode = yuan::plugin::PluginRunMode::script;

        yuan::plugin::LuaScriptPluginAdapter::Config lua_config;
        lua_config.max_instructions_per_call = 100000;

        yuan::plugin::LuaScriptPluginAdapter adapter(manifest, lua_config);
        require(adapter.load_script(tmp + "/main.lua"), "load_script should succeed");

        MockLogger logger;
        MockEventBus bus;
        MockScheduler sched;
        MockStorage storage;
        yuan::app::PluginResourceGuard guard;
        auto ctx = make_test_context(logger, bus, sched, storage, guard);

        bool init_result = adapter.on_init(ctx);
        require(!init_result, "on_init should fail when execution timeout");

        adapter.on_release();
        remove_temp_dir(tmp);
    }

    // ---- test_lua_sandbox_no_c_searcher ----

    void test_lua_sandbox_no_c_searcher()
    {
        std::string tmp = "/tmp/lua_test_csearcher";
        std::string script_content =
            "local plugin = {}\n"
            "function plugin.on_init(ctx)\n"
            "    local count = 0\n"
            "    if package and package.searchers then\n"
            "        for i, s in ipairs(package.searchers) do\n"
            "            count = count + 1\n"
            "        end\n"
            "    end\n"
            "    ctx.logger:info('searchers count = ' .. tostring(count))\n"
            "    return true\n"
            "end\n"
            "function plugin.on_release() end\n"
            "return plugin\n";
        write_temp_script(tmp, "main.lua", script_content);

        yuan::plugin::PluginManifest manifest;
        manifest.name = "lua_test";
        manifest.entry = "main.lua";
        manifest.run_mode = yuan::plugin::PluginRunMode::script;

        yuan::plugin::LuaScriptPluginAdapter adapter(manifest);
        require(adapter.load_script(tmp + "/main.lua"), "load_script should succeed");

        MockLogger logger;
        MockEventBus bus;
        MockScheduler sched;
        MockStorage storage;
        yuan::app::PluginResourceGuard guard;
        auto ctx = make_test_context(logger, bus, sched, storage, guard);

        require(adapter.on_init(ctx), "on_init should succeed");

        bool found_count_2 = false;
        for (auto &m : logger.messages_) {
            if (m.message.find("searchers count = 2") != std::string::npos) {
                found_count_2 = true;
            }
        }
        require(found_count_2, "package.searchers should have only 2 entries (preload + lua)");

        adapter.on_release();
        remove_temp_dir(tmp);
    }

    // ---- test_lua_callback_timeout_eventbus ----

    void test_lua_callback_timeout_eventbus()
    {
        std::string tmp = "/tmp/lua_test_cb_timeout_eb";
        std::string script_content =
            "local plugin = {}\n"
            "function plugin.on_init(ctx)\n"
            "    ctx.event_bus:subscribe('timeout.event', function(event)\n"
            "        local i = 0\n"
            "        while true do i = i + 1 end\n"
            "    end)\n"
            "    return true\n"
            "end\n"
            "function plugin.on_release() end\n"
            "return plugin\n";
        write_temp_script(tmp, "main.lua", script_content);

        yuan::plugin::PluginManifest manifest;
        manifest.name = "lua_test";
        manifest.entry = "main.lua";
        manifest.run_mode = yuan::plugin::PluginRunMode::script;

        yuan::plugin::LuaScriptPluginAdapter::Config lua_config;
        lua_config.max_instructions_per_call = 100000;

        yuan::plugin::LuaScriptPluginAdapter adapter(manifest, lua_config);
        require(adapter.load_script(tmp + "/main.lua"), "load_script should succeed");

        MockLogger logger;
        MockEventBus bus;
        MockScheduler sched;
        MockStorage storage;
        yuan::app::PluginResourceGuard guard;
        auto ctx = make_test_context(logger, bus, sched, storage, guard);

        require(adapter.on_init(ctx), "on_init should succeed");

        bus.publish("timeout.event");

        bool found_timeout = false;
        for (auto &m : logger.messages_) {
            if (m.message.find("instruction limit exceeded") != std::string::npos) {
                found_timeout = true;
            }
        }
        require(found_timeout, "eventbus callback should hit instruction limit and log error");

        adapter.on_release();
        remove_temp_dir(tmp);
    }

    // ---- test_lua_callback_timeout_scheduler ----

    void test_lua_callback_timeout_scheduler()
    {
        std::string tmp = "/tmp/lua_test_cb_timeout_sched";
        std::string script_content =
            "local plugin = {}\n"
            "function plugin.on_init(ctx)\n"
            "    ctx.scheduler:schedule_after(100, function()\n"
            "        local i = 0\n"
            "        while true do i = i + 1 end\n"
            "    end, 'timeout.sched')\n"
            "    return true\n"
            "end\n"
            "function plugin.on_release() end\n"
            "return plugin\n";
        write_temp_script(tmp, "main.lua", script_content);

        yuan::plugin::PluginManifest manifest;
        manifest.name = "lua_test";
        manifest.entry = "main.lua";
        manifest.run_mode = yuan::plugin::PluginRunMode::script;

        yuan::plugin::LuaScriptPluginAdapter::Config lua_config;
        lua_config.max_instructions_per_call = 100000;

        yuan::plugin::LuaScriptPluginAdapter adapter(manifest, lua_config);
        require(adapter.load_script(tmp + "/main.lua"), "load_script should succeed");

        MockLogger logger;
        MockEventBus bus;
        MockScheduler sched;
        MockStorage storage;
        yuan::app::PluginResourceGuard guard;
        auto ctx = make_test_context(logger, bus, sched, storage, guard);

        require(adapter.on_init(ctx), "on_init should succeed");

        require(sched.after_callbacks_.size() >= 1, "should have a scheduled callback");
        sched.after_callbacks_[0].callback();

        bool found_timeout = false;
        for (auto &m : logger.messages_) {
            if (m.message.find("instruction limit exceeded") != std::string::npos) {
                found_timeout = true;
            }
        }
        require(found_timeout, "scheduler callback should hit instruction limit and log error");

        adapter.on_release();
        remove_temp_dir(tmp);
    }


} // namespace

int main()
{
    std::cout << "=== Lua Plugin: Adapter Lifecycle ===" << std::endl;
    test_lua_adapter_lifecycle();
    std::cout << "  PASSED" << std::endl;

    std::cout << "=== Lua Plugin: Logger Binding ===" << std::endl;
    test_lua_bindings_logger();
    std::cout << "  PASSED" << std::endl;

    std::cout << "=== Lua Plugin: EventBus Binding ===" << std::endl;
    test_lua_bindings_event_bus();
    std::cout << "  PASSED" << std::endl;

    std::cout << "=== Lua Plugin: Scheduler Binding ===" << std::endl;
    test_lua_bindings_scheduler();
    std::cout << "  PASSED" << std::endl;

    std::cout << "=== Lua Plugin: Storage Binding ===" << std::endl;
    test_lua_bindings_storage();
    std::cout << "  PASSED" << std::endl;

    std::cout << "=== Lua Plugin: Error Handling ===" << std::endl;
    test_lua_error_handling();
    std::cout << "  PASSED" << std::endl;

    std::cout << "=== Lua Plugin: Sandbox ===" << std::endl;
    test_lua_sandbox();
    std::cout << "  PASSED" << std::endl;

    std::cout << "=== Lua Plugin: Resource Tracking ===" << std::endl;
    test_lua_resource_tracking();
    std::cout << "  PASSED" << std::endl;

    std::cout << "=== Lua Plugin: Missing on_init ===" << std::endl;
    test_lua_script_missing_on_init();
    std::cout << "  PASSED" << std::endl;

    std::cout << "=== Lua Plugin: Script Not Returning Table ===" << std::endl;
    test_lua_script_not_returning_table();
    std::cout << "  PASSED" << std::endl;

    std::cout << "=== Lua Plugin: Config Changed ===" << std::endl;
    test_lua_adapter_config_changed();
    std::cout << "  PASSED" << std::endl;

    std::cout << "=== Lua Plugin: on_init Returns False ===" << std::endl;
    test_lua_on_init_returns_false();
    std::cout << "  PASSED" << std::endl;

    std::cout << "=== Lua Plugin: on_disable Error ===" << std::endl;
    test_lua_on_disable_error();
    std::cout << "  PASSED" << std::endl;

    std::cout << "=== Lua Plugin: Health Check Returns False ===" << std::endl;
    test_lua_health_check_returns_false();
    std::cout << "  PASSED" << std::endl;

    std::cout << "=== Lua Plugin: EventBus Publish With Payload ===" << std::endl;
    test_lua_event_bus_publish_with_payload();
    std::cout << "  PASSED" << std::endl;

    std::cout << "=== Lua Plugin: Memory Budget ===" << std::endl;
    test_lua_memory_budget();
    std::cout << "  PASSED" << std::endl;

    std::cout << "=== Lua Plugin: Execution Timeout ===" << std::endl;
    test_lua_execution_timeout();
    std::cout << "  PASSED" << std::endl;

    std::cout << "=== Lua Plugin: Sandbox No C Searcher ===" << std::endl;
    test_lua_sandbox_no_c_searcher();
    std::cout << "  PASSED" << std::endl;

    std::cout << "=== Lua Plugin: Callback Timeout EventBus ===" << std::endl;
    test_lua_callback_timeout_eventbus();
    std::cout << "  PASSED" << std::endl;

    std::cout << "=== Lua Plugin: Callback Timeout Scheduler ===" << std::endl;
    test_lua_callback_timeout_scheduler();
    std::cout << "  PASSED" << std::endl;

    std::cout << "all lua plugin tests passed" << std::endl;
    return 0;
}
