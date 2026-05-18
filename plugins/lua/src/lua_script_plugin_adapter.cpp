#include "lua_script_plugin_adapter.h"
#include "lua_host_bindings.h"
#include "logger.h"

#include "lua_lib.h"
#include "nlohmann/json.hpp"

#include <cstdlib>
#include <climits>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace yuan::plugin
{

    namespace
    {
        constexpr const char *kLuaPluginTableGlobal = "plugin_table";
        constexpr const char *kLuaStreamConnectionMetaName = "yuan.HostStreamConnection";

        struct PendingLuaStreamPayload
        {
            std::string payload;
            bool consumed = false;
        };

        std::mutex g_pending_stream_payloads_mutex;
        std::unordered_map<const HostStreamConnection *, PendingLuaStreamPayload> g_pending_stream_payloads;

        void set_pending_stream_payload(HostStreamConnection &connection, std::span<const std::byte> bytes)
        {
            PendingLuaStreamPayload payload;
            payload.payload.assign(
                reinterpret_cast<const char *>(bytes.data()),
                reinterpret_cast<const char *>(bytes.data()) + bytes.size());
            std::lock_guard<std::mutex> lock(g_pending_stream_payloads_mutex);
            g_pending_stream_payloads[&connection] = std::move(payload);
        }

        void clear_pending_stream_payload(HostStreamConnection &connection)
        {
            std::lock_guard<std::mutex> lock(g_pending_stream_payloads_mutex);
            g_pending_stream_payloads.erase(&connection);
        }

        bool consume_pending_stream_payload(HostStreamConnection &connection, std::string &payload)
        {
            std::lock_guard<std::mutex> lock(g_pending_stream_payloads_mutex);
            auto it = g_pending_stream_payloads.find(&connection);
            if (it == g_pending_stream_payloads.end() || it->second.consumed) {
                return false;
            }
            payload = it->second.payload;
            it->second.consumed = true;
            return true;
        }

        class LuaStreamProtocolHandler final : public PluginStreamProtocolHandler
        {
        public:
            LuaStreamProtocolHandler(LuaScriptPluginAdapter *adapter, std::string handler_name)
                : adapter_(adapter),
                  handler_name_(std::move(handler_name))
            {
            }

            bool on_data(HostStreamConnection &connection, std::span<const std::byte> bytes) override
            {
                return adapter_ &&
                       adapter_->call_stream_protocol_handler(handler_name_, connection, bytes);
            }

        private:
            LuaScriptPluginAdapter *adapter_ = nullptr;
            std::string handler_name_;
        };

        HostStreamConnection *check_stream_connection_userdata(lua_State *L, int index)
        {
            auto **ud = static_cast<void **>(luaL_checkudata(L, index, kLuaStreamConnectionMetaName));
            return ud ? static_cast<HostStreamConnection *>(*ud) : nullptr;
        }

        int lua_stream_connection_id(lua_State *L)
        {
            auto *connection = check_stream_connection_userdata(L, 1);
            lua_pushinteger(L, static_cast<lua_Integer>(connection ? connection->id() : 0));
            return 1;
        }

        int lua_stream_connection_peer_address(lua_State *L)
        {
            auto *connection = check_stream_connection_userdata(L, 1);
            if (!connection) {
                lua_pushliteral(L, "");
                return 1;
            }
            const auto peer = connection->peer_address();
            lua_pushlstring(L, peer.c_str(), peer.size());
            return 1;
        }

        int lua_stream_connection_local_address(lua_State *L)
        {
            auto *connection = check_stream_connection_userdata(L, 1);
            if (!connection) {
                lua_pushliteral(L, "");
                return 1;
            }
            const auto local = connection->local_address();
            lua_pushlstring(L, local.c_str(), local.size());
            return 1;
        }

        int lua_stream_connection_write(lua_State *L)
        {
            auto *connection = check_stream_connection_userdata(L, 1);
            size_t len = 0;
            const char *bytes = luaL_checklstring(L, 2, &len);
            if (!connection) {
                lua_pushboolean(L, 0);
                return 1;
            }
            const auto *ptr = reinterpret_cast<const std::byte *>(bytes);
            lua_pushboolean(L, connection->write(std::span<const std::byte>(ptr, len)) ? 1 : 0);
            return 1;
        }

        int lua_stream_connection_read(lua_State *L)
        {
            auto *connection = check_stream_connection_userdata(L, 1);
            if (lua_gettop(L) >= 2) {
                (void)lua_tointeger(L, 2);
            }

            if (!connection) {
                lua_pushnil(L);
                return 1;
            }

            std::string payload;
            if (!consume_pending_stream_payload(*connection, payload)) {
                lua_pushnil(L);
                return 1;
            }

            lua_pushlstring(L, payload.data(), payload.size());
            return 1;
        }

        int lua_stream_connection_read_line(lua_State *L)
        {
            auto *connection = check_stream_connection_userdata(L, 1);
            if (lua_gettop(L) >= 2) {
                (void)lua_tointeger(L, 2);
            }

            if (!connection) {
                lua_pushnil(L);
                return 1;
            }

            std::string payload;
            if (!consume_pending_stream_payload(*connection, payload)) {
                lua_pushnil(L);
                return 1;
            }

            if (!payload.empty() && payload.back() == '\n') {
                payload.pop_back();
            }
            if (!payload.empty() && payload.back() == '\r') {
                payload.pop_back();
            }

            lua_pushlstring(L, payload.data(), payload.size());
            return 1;
        }

        int lua_stream_connection_flush(lua_State *L)
        {
            auto *connection = check_stream_connection_userdata(L, 1);
            lua_pushboolean(L, (connection && connection->flush()) ? 1 : 0);
            return 1;
        }

        int lua_stream_connection_close(lua_State *L)
        {
            auto *connection = check_stream_connection_userdata(L, 1);
            if (connection) {
                connection->close();
            }
            return 0;
        }

        int lua_stream_connection_is_open(lua_State *L)
        {
            auto *connection = check_stream_connection_userdata(L, 1);
            lua_pushboolean(L, (connection && connection->is_open()) ? 1 : 0);
            return 1;
        }

        void ensure_stream_connection_metatable(lua_State *L)
        {
            if (luaL_newmetatable(L, kLuaStreamConnectionMetaName) == 1) {
                static const luaL_Reg methods[] = {
                    { "id", lua_stream_connection_id },
                    { "peer_address", lua_stream_connection_peer_address },
                    { "local_address", lua_stream_connection_local_address },
                    { "read", lua_stream_connection_read },
                    { "read_line", lua_stream_connection_read_line },
                    { "write", lua_stream_connection_write },
                    { "flush", lua_stream_connection_flush },
                    { "close", lua_stream_connection_close },
                    { "is_open", lua_stream_connection_is_open },
                    { nullptr, nullptr }
                };
                lua_pushvalue(L, -1);
                lua_setfield(L, -2, "__index");
                luaL_setfuncs(L, methods, 0);
            }
            lua_pop(L, 1);
        }

        void push_stream_connection_userdata(lua_State *L, HostStreamConnection &connection)
        {
            ensure_stream_connection_metatable(L);
            auto **ud = static_cast<void **>(lua_newuserdata(L, sizeof(void *)));
            *ud = static_cast<void *>(&connection);
            luaL_setmetatable(L, kLuaStreamConnectionMetaName);
        }

        std::string fallback_handler_name(const std::string &handler_name)
        {
            const auto pos = handler_name.find_last_of('.');
            if (pos == std::string::npos || pos + 1 >= handler_name.size()) {
                return {};
            }
            return handler_name.substr(pos + 1);
        }

        bool resolve_handler_function(lua_State *L,
                                      const std::string &handler_name,
                                      bool *function_missing)
        {
            if (function_missing) {
                *function_missing = false;
            }

            lua_getglobal(L, kLuaPluginTableGlobal);
            if (!lua_istable(L, -1)) {
                lua_pop(L, 1);
                if (function_missing) {
                    *function_missing = true;
                }
                return false;
            }

            lua_getfield(L, -1, handler_name.c_str());
            if (lua_isfunction(L, -1)) {
                lua_remove(L, -2);
                return true;
            }
            lua_pop(L, 1);

            const auto fallback = fallback_handler_name(handler_name);
            if (!fallback.empty()) {
                lua_getfield(L, -1, fallback.c_str());
                if (lua_isfunction(L, -1)) {
                    lua_remove(L, -2);
                    return true;
                }
                lua_pop(L, 1);
            }

            lua_pop(L, 1);
            if (function_missing) {
                *function_missing = true;
            }
            return false;
        }
    } // namespace

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

        lua_setglobal(L_, kLuaPluginTableGlobal);

        script_loaded_ = true;
        return true;
    }

    void LuaScriptPluginAdapter::call_lua_void(const char * func_name)
    {
        std::lock_guard<std::recursive_mutex> lock(lua_mutex_);
        if (!L_)
            return;

        lua_getglobal(L_, kLuaPluginTableGlobal);
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

        lua_getglobal(L_, kLuaPluginTableGlobal);
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
            if (err && std::string_view(err).find("instruction limit exceeded") != std::string_view::npos) {
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

        lua_getglobal(L_, kLuaPluginTableGlobal);
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

        lua_getglobal(L_, kLuaPluginTableGlobal);
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

    void LuaScriptPluginAdapter::register_protocol_handlers(PluginProtocolHandlerRegistry &registry)
    {
        std::unordered_set<std::string> seen_handlers;
        for (const auto &service : manifest_.protocol_services) {
            if (service.handler.empty() || service.language != "lua") {
                continue;
            }

            if (!seen_handlers.insert(service.handler).second) {
                continue;
            }

            const std::string handler_name = service.handler;
            if (!has_stream_protocol_handler(handler_name)) {
                LOG_WARN("lua plugin '{}' skipped protocol handler '{}' because function is missing",
                         manifest_.name,
                         handler_name);
                continue;
            }
            (void)registry.register_stream_handler(
                handler_name,
                [this, handler_name](const ProtocolServiceDescriptor &) {
                    return std::make_unique<LuaStreamProtocolHandler>(this, handler_name);
                });
        }
    }

    bool LuaScriptPluginAdapter::has_stream_protocol_handler(const std::string &handler_name) const
    {
        std::lock_guard<std::recursive_mutex> lock(lua_mutex_);
        if (!L_) {
            return false;
        }

        bool function_missing = false;
        const bool found = resolve_handler_function(L_, handler_name, &function_missing);
        if (found) {
            lua_pop(L_, 1);
        }
        return found && !function_missing;
    }

    bool LuaScriptPluginAdapter::call_stream_protocol_handler(const std::string &handler_name,
                                                              HostStreamConnection &connection,
                                                              std::span<const std::byte> bytes)
    {
        return call_lua_stream_handler(handler_name, connection, bytes, nullptr);
    }

    bool LuaScriptPluginAdapter::call_lua_stream_handler(const std::string &handler_name,
                                                         HostStreamConnection &connection,
                                                         std::span<const std::byte> bytes,
                                                         bool *function_missing) const
    {
        std::lock_guard<std::recursive_mutex> lock(lua_mutex_);
        if (!L_) {
            if (function_missing) {
                *function_missing = true;
            }
            return false;
        }

        if (!resolve_handler_function(L_, handler_name, function_missing)) {
            return false;
        }

        set_pending_stream_payload(connection, bytes);
        push_stream_connection_userdata(L_, connection);
        const auto *payload = reinterpret_cast<const char *>(bytes.data());
        lua_pushlstring(L_, payload ? payload : "", bytes.size());

        set_execution_hook();
        const int ret = lua_pcall(L_, 2, 1, 0);
        clear_execution_hook();

        if (ret != LUA_OK) {
            const char *err = lua_tostring(L_, -1);
            std::string message = "lua plugin '" + manifest_.name + "' protocol handler '" +
                                  handler_name + "' error: " + (err ? err : "unknown");
            LOG_ERROR("{}", message);
            log_host_error(message);
            lua_pop(L_, 1);
            clear_pending_stream_payload(connection);
            return false;
        }

        bool keep_open = true;
        if (lua_isboolean(L_, -1)) {
            keep_open = lua_toboolean(L_, -1) != 0;
        } else if (lua_isnil(L_, -1)) {
            keep_open = true;
        } else {
            keep_open = lua_toboolean(L_, -1) != 0;
        }
        lua_pop(L_, 1);
        clear_pending_stream_payload(connection);
        return keep_open;
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
