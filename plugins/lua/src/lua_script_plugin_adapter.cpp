#include "lua_script_plugin_adapter.h"
#include "lua_host_bindings.h"
#include "logger.h"

#include "lua_lib.h"
#include "nlohmann/json.hpp"

#include <climits>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace yuan::plugin
{

    LuaScriptPluginAdapter::LuaScriptPluginAdapter(const PluginManifest & manifest)
        : ScriptPluginAdapter(manifest)
    {
        memory_budget_.limit = config_.memory_budget_bytes;
    }

    LuaScriptPluginAdapter::LuaScriptPluginAdapter(const PluginManifest & manifest, const Config & config)
        : ScriptPluginAdapter(manifest), config_(config)
    {
        memory_budget_.limit = config_.memory_budget_bytes;
    }

    LuaScriptPluginAdapter::~LuaScriptPluginAdapter()
    {
        std::lock_guard<std::recursive_mutex> lock(lua_mutex_);
        if (L_) {
            cleanup_lua_plugin_callbacks(L_, callback_owner_name());
            lua_close(L_);
            L_ = nullptr;
        }
    }

    void *LuaScriptPluginAdapter::lua_budget_allocator(void * ud, void * ptr, size_t osize, size_t nsize)
    {
        auto *budget = static_cast<LuaMemoryBudget *>(ud);
        if (nsize == 0) {
            if (ptr) {
                if (osize > budget->used) {
                    budget->used = 0;
                } else {
                    budget->used -= osize;
                }
            }
            return nullptr;
        }

        if (!ptr) {
            if (budget->used + nsize > budget->limit) {
                return nullptr;
            }
            budget->used += nsize;
            return malloc(nsize);
        }

        size_t delta = 0;
        if (nsize > osize) {
            delta = nsize - osize;
            if (budget->used + delta > budget->limit) {
                return nullptr;
            }
        }

        void *new_ptr = realloc(ptr, nsize);
        if (new_ptr) {
            if (nsize > osize) {
                budget->used += delta;
            } else {
                if (osize - nsize > budget->used) {
                    budget->used = 0;
                } else {
                    budget->used -= (osize - nsize);
                }
            }
        }
        return new_ptr;
    }

    void LuaScriptPluginAdapter::lua_execution_timeout_hook(lua_State * L, lua_Debug * /*ar*/)
    {
        luaL_error(L, "instruction limit exceeded");
    }

    void LuaScriptPluginAdapter::set_execution_hook() const
    {
        if (config_.max_instructions_per_call > 0 && config_.max_instructions_per_call < SIZE_MAX) {
            int count = (config_.max_instructions_per_call > static_cast<size_t>(INT_MAX))
                            ? INT_MAX
                            : static_cast<int>(config_.max_instructions_per_call);
            lua_sethook(L_, lua_execution_timeout_hook, LUA_MASKCOUNT, count);
        }
    }

    void LuaScriptPluginAdapter::clear_execution_hook() const
    {
        lua_sethook(L_, nullptr, 0, 0);
    }

    bool LuaScriptPluginAdapter::init_lua_state()
    {
        L_ = lua_newstate(lua_budget_allocator, &memory_budget_);
        if (!L_) {
            LOG_ERROR("failed to create lua state for plugin '{}'", manifest_.name);
            return false;
        }

        luaL_openlibs(L_);
        apply_sandbox();
        return true;
    }

    void LuaScriptPluginAdapter::apply_sandbox()
    {
        lua_pushnil(L_);
        lua_setglobal(L_, "io");

        lua_pushnil(L_);
        lua_setglobal(L_, "debug");

        lua_getglobal(L_, "os");
        if (lua_istable(L_, -1)) {
            const char *os_dangerous[] = { "execute", "exit", "getenv", "remove", "rename", "tmpname" };
            for (const char *name : os_dangerous) {
                lua_pushnil(L_);
                lua_setfield(L_, -2, name);
            }
        }
        lua_pop(L_, 1);

        lua_getglobal(L_, "package");
        if (lua_istable(L_, -1)) {
            lua_pushnil(L_);
            lua_setfield(L_, -2, "loadlib");

            lua_pushnil(L_);
            lua_setfield(L_, -2, "cpath");

            lua_getfield(L_, -1, "searchers");
            if (lua_istable(L_, -1)) {
                int len = static_cast<int>(lua_rawlen(L_, -1));
                for (int i = len; i >= 3; --i) {
                    lua_pushnil(L_);
                    lua_rawseti(L_, -2, i);
                }
            }
            lua_pop(L_, 1);
        }
        lua_pop(L_, 1);
    }

    bool LuaScriptPluginAdapter::load_script(const std::string & script_path)
    {
        if (!init_lua_state()) {
            return false;
        }

        std::ifstream file(script_path);
        if (!file.good()) {
            LOG_ERROR("failed to open lua script '{}'", script_path);
            lua_close(L_);
            L_ = nullptr;
            return false;
        }

        std::ostringstream ss;
        ss << file.rdbuf();
        std::string script_content = ss.str();

        if (luaL_loadbuffer(L_, script_content.c_str(), script_content.size(), script_path.c_str()) != LUA_OK) {
            const char *err = lua_tostring(L_, -1);
            LOG_ERROR("failed to parse lua script '{}': {}", script_path, err ? err : "unknown");
            lua_pop(L_, 1);
            lua_close(L_);
            L_ = nullptr;
            return false;
        }

        set_execution_hook();
        int ret = lua_pcall(L_, 0, 1, 0);
        clear_execution_hook();

        if (ret != LUA_OK) {
            const char *err = lua_tostring(L_, -1);
            LOG_ERROR("failed to execute lua script '{}': {}", script_path, err ? err : "unknown");
            lua_pop(L_, 1);
            lua_close(L_);
            L_ = nullptr;
            return false;
        }

        if (!lua_istable(L_, -1)) {
            LOG_ERROR("lua script '{}' must return a table", script_path);
            lua_pop(L_, 1);
            lua_close(L_);
            L_ = nullptr;
            return false;
        }

        lua_getfield(L_, -1, "on_init");
        if (!lua_isfunction(L_, -1)) {
            LOG_ERROR("lua script '{}' must define on_init function", script_path);
            lua_pop(L_, 2);
            lua_close(L_);
            L_ = nullptr;
            return false;
        }
        lua_pop(L_, 1);

        lua_setglobal(L_, "plugin_table");

        script_loaded_ = true;
        return true;
    }

    void LuaScriptPluginAdapter::call_lua_void(const char * func_name)
    {
        std::lock_guard<std::recursive_mutex> lock(lua_mutex_);
        if (!L_)
            return;

        lua_getglobal(L_, "plugin_table");
        if (!lua_istable(L_, -1)) {
            lua_pop(L_, 1);
            return;
        }

        lua_getfield(L_, -1, func_name);
        if (!lua_isfunction(L_, -1)) {
            lua_pop(L_, 2);
            return;
        }

        set_execution_hook();
        if (lua_pcall(L_, 0, 0, 0) != LUA_OK) {
            const char *err = lua_tostring(L_, -1);
            std::string message = "lua plugin '" + manifest_.name + "' " + func_name + ": "
                                + (err ? err : "unknown");
            LOG_ERROR("{}", message);
            log_host_error(message);
            lua_pop(L_, 1);
        }
        clear_execution_hook();

        lua_pop(L_, 1);
    }

    bool LuaScriptPluginAdapter::call_lua_init(const PluginContext & context)
    {
        std::lock_guard<std::recursive_mutex> lock(lua_mutex_);
        if (!L_)
            return false;

        lua_getglobal(L_, "plugin_table");
        if (!lua_istable(L_, -1)) {
            lua_pop(L_, 1);
            return false;
        }

        lua_getfield(L_, -1, "on_init");
        if (!lua_isfunction(L_, -1)) {
            lua_pop(L_, 2);
            return false;
        }

        lua_register_host_modules(L_, context, config_.max_instructions_per_call, &lua_mutex_);

        set_execution_hook();
        timeout_triggered_ = false;
        int ret = lua_pcall(L_, 1, 1, 0);
        clear_execution_hook();

        if (ret != LUA_OK) {
            const char *err = lua_tostring(L_, -1);
            std::string message = "lua plugin '" + manifest_.name + "' on_init error: "
                                + (err ? err : "unknown");
            LOG_ERROR("{}", message);
            log_host_error(message);
            if (err && strstr(err, "instruction limit exceeded")) {
                timeout_triggered_ = true;
            }
            lua_pop(L_, 2);
            return false;
        }

        bool result = lua_toboolean(L_, -1) != 0;
        lua_pop(L_, 2);
        return result;
    }

    void LuaScriptPluginAdapter::call_lua_config_changed(const PluginConfigView & config)
    {
        std::lock_guard<std::recursive_mutex> lock(lua_mutex_);
        if (!L_)
            return;

        lua_getglobal(L_, "plugin_table");
        if (!lua_istable(L_, -1)) {
            lua_pop(L_, 1);
            return;
        }

        lua_getfield(L_, -1, "on_config_changed");
        if (!lua_isfunction(L_, -1)) {
            lua_pop(L_, 2);
            return;
        }

        auto *raw = config.raw();
        if (raw && raw->is_object()) {
            push_json_to_lua(L_, *raw);
        } else {
            lua_pushnil(L_);
        }

        set_execution_hook();
        if (lua_pcall(L_, 1, 0, 0) != LUA_OK) {
            const char *err = lua_tostring(L_, -1);
            std::string message = "lua plugin '" + manifest_.name + "' on_config_changed error: "
                                + (err ? err : "unknown");
            LOG_ERROR("{}", message);
            log_host_error(message);
            lua_pop(L_, 1);
        }
        clear_execution_hook();

        lua_pop(L_, 1);
    }

    bool LuaScriptPluginAdapter::do_init(const PluginContext & context)
    {
        return call_lua_init(context);
    }

    void LuaScriptPluginAdapter::do_enable()
    {
        call_lua_void("on_enable");
    }

    void LuaScriptPluginAdapter::do_disable()
    {
        call_lua_void("on_disable");
    }

    void LuaScriptPluginAdapter::do_release()
    {
        std::lock_guard<std::recursive_mutex> lock(lua_mutex_);
        call_lua_void("on_release");
        if (L_) {
            cleanup_lua_plugin_callbacks(L_, callback_owner_name());
            lua_close(L_);
            L_ = nullptr;
        }
    }

    bool LuaScriptPluginAdapter::do_health_check() const
    {
        std::lock_guard<std::recursive_mutex> lock(lua_mutex_);
        if (!L_) {
            return true;
        }

        lua_getglobal(L_, "plugin_table");
        if (!lua_istable(L_, -1)) {
            lua_pop(L_, 1);
            return true;
        }

        lua_getfield(L_, -1, "on_health_check");
        if (!lua_isfunction(L_, -1)) {
            lua_pop(L_, 2);
            return true;
        }

        set_execution_hook();
        int ret = lua_pcall(L_, 0, 1, 0);
        clear_execution_hook();

        if (ret != LUA_OK) {
            const char *err = lua_tostring(L_, -1);
            std::string message = "lua plugin '" + manifest_.name + "' on_health_check error: "
                                + (err ? err : "unknown");
            LOG_ERROR("{}", message);
            log_host_error(message);
            lua_pop(L_, 2);
            return false;
        }

        bool result = lua_toboolean(L_, -1) != 0;
        lua_pop(L_, 2);
        return result;
    }

    void LuaScriptPluginAdapter::do_config_changed(const PluginConfigView & config)
    {
        call_lua_config_changed(config);
    }

    void LuaScriptPluginAdapter::log_host_error(std::string_view message) const
    {
        if (context_.logger) {
            context_.logger->log(
                HostLogLevel::error,
                __FILE__,
                __LINE__,
                __func__,
                message);
        }
    }

    std::string LuaScriptPluginAdapter::callback_owner_name() const
    {
        if (!context_.plugin_name.empty()) {
            return context_.plugin_name;
        }
        if (!manifest_.plugin_id.empty()) {
            return manifest_.plugin_id;
        }
        return manifest_.name;
    }

} // namespace yuan::plugin
