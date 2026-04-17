#include "lua_host_bindings.h"
#include "plugin/host_logger.h"
#include "plugin/host_event_bus.h"
#include "plugin/host_scheduler.h"
#include "plugin/host_storage.h"
#include "plugin/host_resource_guard.h"
#include "plugin/plugin_context.h"
#include "logger.h"

#include "lua_lib.h"

#include <any>
#include <climits>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace yuan::plugin
{

    namespace
    {

        static const char *CTX_REGISTRY_KEY = "_yuan_plugin_ctx_info";

        struct LuaCtxInfo
        {
            std::string plugin_name;
            size_t max_instructions_per_call;
            HostResourceGuard *resource_guard;
            std::recursive_mutex *lua_mutex;
        };

        struct LuaEventCallback
        {
            lua_State *L;
            int ref;
            std::string plugin_name;
            size_t max_instructions_per_call;
            std::recursive_mutex *lua_mutex;
        };

        struct LuaSchedulerCallback
        {
            lua_State *L;
            int ref;
            std::string plugin_name;
            size_t max_instructions_per_call;
            std::recursive_mutex *lua_mutex;
        };

        struct LuaEventPayload
        {
            std::any payload;
        };

        static std::unordered_map<int, LuaEventCallback> g_event_callbacks;
        static std::unordered_map<int, LuaSchedulerCallback> g_scheduler_callbacks;
        static int g_next_callback_id = 1;
        static std::mutex g_callbacks_mutex;

        static LuaCtxInfo *get_ctx_info(lua_State *L)
        {
            lua_pushstring(L, CTX_REGISTRY_KEY);
            lua_rawget(L, LUA_REGISTRYINDEX);
            if (lua_islightuserdata(L, -1)) {
                auto *info = static_cast<LuaCtxInfo *>(lua_touserdata(L, -1));
                lua_pop(L, 1);
                return info;
            }
            lua_pop(L, 1);
            return nullptr;
        }

        static void set_callback_hook(lua_State *L, size_t max_instructions)
        {
            if (max_instructions > 0 && max_instructions < SIZE_MAX) {
                int count = (max_instructions > static_cast<size_t>(INT_MAX))
                                ? INT_MAX
                                : static_cast<int>(max_instructions);
                lua_sethook(L, [](lua_State *ls, lua_Debug *ar) {
                    (void)ar;
                    luaL_error(ls, "instruction limit exceeded in callback");
                               },
                            LUA_MASKCOUNT, count);
            }
        }

        static void clear_callback_hook(lua_State *L)
        {
            lua_sethook(L, nullptr, 0, 0);
        }

        static HostLogLevel parse_log_level(const char *level_str)
        {
            if (strcmp(level_str, "trace") == 0)
                return HostLogLevel::trace;
            if (strcmp(level_str, "debug") == 0)
                return HostLogLevel::debug;
            if (strcmp(level_str, "info") == 0)
                return HostLogLevel::info;
            if (strcmp(level_str, "warn") == 0)
                return HostLogLevel::warn;
            if (strcmp(level_str, "error") == 0)
                return HostLogLevel::error;
            if (strcmp(level_str, "fatal") == 0)
                return HostLogLevel::fatal;
            return HostLogLevel::info;
        }

        // ---- Logger binding ----

        static int lua_logger_log(lua_State *L)
        {
            auto *logger = static_cast<HostLogger *>(luaL_checkudata(L, 1, "yuan.HostLogger"));
            const char *level_str = luaL_checkstring(L, 2);
            const char *msg = luaL_checkstring(L, 3);
            if (logger) {
                logger->log(parse_log_level(level_str), "", 0, "", std::string_view(msg));
            }
            return 0;
        }

        static int lua_logger_info(lua_State *L)
        {
            auto *logger = static_cast<HostLogger *>(luaL_checkudata(L, 1, "yuan.HostLogger"));
            const char *msg = luaL_checkstring(L, 2);
            if (logger) {
                logger->log(HostLogLevel::info, "", 0, "", std::string_view(msg));
            }
            return 0;
        }

        static int lua_logger_warn(lua_State *L)
        {
            auto *logger = static_cast<HostLogger *>(luaL_checkudata(L, 1, "yuan.HostLogger"));
            const char *msg = luaL_checkstring(L, 2);
            if (logger) {
                logger->log(HostLogLevel::warn, "", 0, "", std::string_view(msg));
            }
            return 0;
        }

        static int lua_logger_error(lua_State *L)
        {
            auto *logger = static_cast<HostLogger *>(luaL_checkudata(L, 1, "yuan.HostLogger"));
            const char *msg = luaL_checkstring(L, 2);
            if (logger) {
                logger->log(HostLogLevel::error, "", 0, "", std::string_view(msg));
            }
            return 0;
        }

        static int lua_logger_debug(lua_State *L)
        {
            auto *logger = static_cast<HostLogger *>(luaL_checkudata(L, 1, "yuan.HostLogger"));
            const char *msg = luaL_checkstring(L, 2);
            if (logger) {
                logger->log(HostLogLevel::debug, "", 0, "", std::string_view(msg));
            }
            return 0;
        }

        static int lua_logger_trace(lua_State *L)
        {
            auto *logger = static_cast<HostLogger *>(luaL_checkudata(L, 1, "yuan.HostLogger"));
            const char *msg = luaL_checkstring(L, 2);
            if (logger) {
                logger->log(HostLogLevel::trace, "", 0, "", std::string_view(msg));
            }
            return 0;
        }

        static int lua_logger_fatal(lua_State *L)
        {
            auto *logger = static_cast<HostLogger *>(luaL_checkudata(L, 1, "yuan.HostLogger"));
            const char *msg = luaL_checkstring(L, 2);
            if (logger) {
                logger->log(HostLogLevel::fatal, "", 0, "", std::string_view(msg));
            }
            return 0;
        }

        static const luaL_Reg logger_methods[] = {
            { "log", lua_logger_log },
            { "info", lua_logger_info },
            { "warn", lua_logger_warn },
            { "error", lua_logger_error },
            { "debug", lua_logger_debug },
            { "trace", lua_logger_trace },
            { "fatal", lua_logger_fatal },
            { nullptr, nullptr }
        };

        // ---- Storage binding ----

        static int lua_storage_set(lua_State *L)
        {
            auto *storage = static_cast<HostStorage *>(luaL_checkudata(L, 1, "yuan.HostStorage"));
            const char *key = luaL_checkstring(L, 2);
            const char *value = luaL_checkstring(L, 3);
            if (storage) {
                lua_pushboolean(L, storage->set(key, value) ? 1 : 0);
            } else {
                lua_pushboolean(L, 0);
            }
            return 1;
        }

        static int lua_storage_get(lua_State *L)
        {
            auto *storage = static_cast<HostStorage *>(luaL_checkudata(L, 1, "yuan.HostStorage"));
            const char *key = luaL_checkstring(L, 2);
            if (storage) {
                auto val = storage->get(key);
                if (val) {
                    lua_pushstring(L, val->c_str());
                } else {
                    lua_pushnil(L);
                }
            } else {
                lua_pushnil(L);
            }
            return 1;
        }

        static int lua_storage_del(lua_State *L)
        {
            auto *storage = static_cast<HostStorage *>(luaL_checkudata(L, 1, "yuan.HostStorage"));
            const char *key = luaL_checkstring(L, 2);
            if (storage) {
                lua_pushboolean(L, storage->del(key) ? 1 : 0);
            } else {
                lua_pushboolean(L, 0);
            }
            return 1;
        }

        static int lua_storage_exists(lua_State *L)
        {
            auto *storage = static_cast<HostStorage *>(luaL_checkudata(L, 1, "yuan.HostStorage"));
            const char *key = luaL_checkstring(L, 2);
            if (storage) {
                lua_pushboolean(L, storage->exists(key) ? 1 : 0);
            } else {
                lua_pushboolean(L, 0);
            }
            return 1;
        }

        static int lua_storage_hset(lua_State *L)
        {
            auto *storage = static_cast<HostStorage *>(luaL_checkudata(L, 1, "yuan.HostStorage"));
            const char *key = luaL_checkstring(L, 2);
            const char *field = luaL_checkstring(L, 3);
            const char *value = luaL_checkstring(L, 4);
            if (storage) {
                lua_pushboolean(L, storage->hset(key, field, value) ? 1 : 0);
            } else {
                lua_pushboolean(L, 0);
            }
            return 1;
        }

        static int lua_storage_hget(lua_State *L)
        {
            auto *storage = static_cast<HostStorage *>(luaL_checkudata(L, 1, "yuan.HostStorage"));
            const char *key = luaL_checkstring(L, 2);
            const char *field = luaL_checkstring(L, 3);
            if (storage) {
                auto val = storage->hget(key, field);
                if (val) {
                    lua_pushstring(L, val->c_str());
                } else {
                    lua_pushnil(L);
                }
            } else {
                lua_pushnil(L);
            }
            return 1;
        }

        static int lua_storage_hdel(lua_State *L)
        {
            auto *storage = static_cast<HostStorage *>(luaL_checkudata(L, 1, "yuan.HostStorage"));
            const char *key = luaL_checkstring(L, 2);
            const char *field = luaL_checkstring(L, 3);
            if (storage) {
                lua_pushboolean(L, storage->hdel(key, field) ? 1 : 0);
            } else {
                lua_pushboolean(L, 0);
            }
            return 1;
        }

        static int lua_storage_hgetall(lua_State *L)
        {
            auto *storage = static_cast<HostStorage *>(luaL_checkudata(L, 1, "yuan.HostStorage"));
            const char *key = luaL_checkstring(L, 2);
            if (storage) {
                auto map = storage->hgetall(key);
                lua_createtable(L, 0, static_cast<int>(map.size()));
                for (auto & [
                                k,
                                v
                            ] : map) {
                    lua_pushstring(L, k.c_str());
                    lua_pushstring(L, v.c_str());
                    lua_rawset(L, -3);
                }
            } else {
                lua_createtable(L, 0, 0);
            }
            return 1;
        }

        static int lua_storage_is_available(lua_State *L)
        {
            auto *storage = static_cast<HostStorage *>(luaL_checkudata(L, 1, "yuan.HostStorage"));
            if (storage) {
                lua_pushboolean(L, storage->is_available() ? 1 : 0);
            } else {
                lua_pushboolean(L, 0);
            }
            return 1;
        }

        static const luaL_Reg storage_methods[] = {
            { "set", lua_storage_set },
            { "get", lua_storage_get },
            { "del", lua_storage_del },
            { "exists", lua_storage_exists },
            { "hset", lua_storage_hset },
            { "hget", lua_storage_hget },
            { "hdel", lua_storage_hdel },
            { "hgetall", lua_storage_hgetall },
            { "is_available", lua_storage_is_available },
            { nullptr, nullptr }
        };

        // ---- JSON <-> Lua helpers ----

        static void push_json_value(lua_State *L, const nlohmann::json &val);

        static void push_json_table(lua_State *L, const nlohmann::json &j)
        {
            lua_createtable(L, 0, static_cast<int>(j.size()));
            for (auto it = j.begin(); it != j.end(); ++it) {
                lua_pushstring(L, it.key().c_str());
                push_json_value(L, it.value());
                lua_rawset(L, -3);
            }
        }

        static void push_json_value(lua_State *L, const nlohmann::json &val)
        {
            if (val.is_null()) {
                lua_pushnil(L);
            } else if (val.is_boolean()) {
                lua_pushboolean(L, val.get<bool>() ? 1 : 0);
            } else if (val.is_number_integer()) {
                lua_pushinteger(L, val.get<lua_Integer>());
            } else if (val.is_number_float()) {
                lua_pushnumber(L, val.get<lua_Number>());
            } else if (val.is_string()) {
                lua_pushstring(L, val.get_ref<const std::string &>().c_str());
            } else if (val.is_array()) {
                lua_createtable(L, static_cast<int>(val.size()), 0);
                for (size_t i = 0; i < val.size(); ++i) {
                    lua_pushinteger(L, static_cast<lua_Integer>(i + 1));
                    push_json_value(L, val[i]);
                    lua_rawset(L, -3);
                }
            } else if (val.is_object()) {
                push_json_table(L, val);
            } else {
                lua_pushnil(L);
            }
        }

        // ---- EventBus binding ----

        static int lua_eventbus_subscribe(lua_State *L)
        {
            auto *bus = static_cast<HostEventBus *>(luaL_checkudata(L, 1, "yuan.HostEventBus"));
            const char *event_name = luaL_checkstring(L, 2);
            luaL_checktype(L, 3, LUA_TFUNCTION);

            lua_pushvalue(L, 3);
            int ref = luaL_ref(L, LUA_REGISTRYINDEX);

            auto *ctx_info = get_ctx_info(L);

            LuaEventCallback cb;
            cb.L = L;
            cb.ref = ref;
            cb.plugin_name = ctx_info ? ctx_info->plugin_name : "";
            cb.max_instructions_per_call = ctx_info ? ctx_info->max_instructions_per_call : 0;
            cb.lua_mutex = ctx_info ? ctx_info->lua_mutex : nullptr;

            int cb_id;
            {
                std::lock_guard<std::mutex> lock(g_callbacks_mutex);
                cb_id = g_next_callback_id++;
                g_event_callbacks[cb_id] = std::move(cb);
            }

            if (bus) {
                auto token = bus->subscribe(event_name, [cb_id](const HostEvent &event) {
                    LuaEventCallback ecb;
                    {
                        std::lock_guard<std::mutex> lock(g_callbacks_mutex);
                        auto it = g_event_callbacks.find(cb_id);
                        if (it == g_event_callbacks.end()) return;
                        ecb = it->second;
                    }

                    if (!ecb.lua_mutex) return;

                    std::lock_guard<std::recursive_mutex> lua_lock(*ecb.lua_mutex);
                    lua_State *ls = ecb.L;

                    lua_rawgeti(ls, LUA_REGISTRYINDEX, ecb.ref);

                    lua_createtable(ls, 0, 2);
                    lua_pushstring(ls, "name");
                    lua_pushstring(ls, event.name.c_str());
                    lua_rawset(ls, -3);

                    lua_pushstring(ls, "payload");
                    if (event.payload.has_value()) {
                        auto *payload_ptr = std::any_cast<LuaEventPayload>(&event.payload);
                        if (payload_ptr && payload_ptr->payload.has_value()) {
                            auto *json_ptr = std::any_cast<nlohmann::json>(&payload_ptr->payload);
                            if (json_ptr) {
                                push_json_table(ls, *json_ptr);
                            } else {
                                lua_pushnil(ls);
                            }
                        } else {
                            lua_pushnil(ls);
                        }
                    } else {
                        lua_pushnil(ls);
                    }
                    lua_rawset(ls, -3);

                    set_callback_hook(ls, ecb.max_instructions_per_call);
                    if (lua_pcall(ls, 1, 0, 0) != LUA_OK) {
                        const char *err = lua_tostring(ls, -1);
                        LOG_ERROR("lua event callback error: {}", err ? err : "unknown");
                        lua_pop(ls, 1);
                    }
                    clear_callback_hook(ls);
                });

                auto *guard = ctx_info ? ctx_info->resource_guard : nullptr;
                if (guard) {
                    guard->track(cb.plugin_name, PluginResourceType::event_subscription,
                                 [bus, token]() {
                                     if (bus) bus->unsubscribe(token);
                                 },
                                 "event:" + std::string(event_name));
                }
            }

            return 0;
        }

        static int lua_eventbus_publish(lua_State *L)
        {
            auto *bus = static_cast<HostEventBus *>(luaL_checkudata(L, 1, "yuan.HostEventBus"));
            const char *event_name = luaL_checkstring(L, 2);

            if (bus) {
                if (lua_istable(L, 3)) {
                    nlohmann::json payload;
                    lua_pushnil(L);
                    while (lua_next(L, 3) != 0) {
                        std::string key;
                        if (lua_type(L, -2) == LUA_TSTRING) {
                            key = lua_tostring(L, -2);
                        }
                        if (lua_type(L, -1) == LUA_TSTRING) {
                            payload[key] = lua_tostring(L, -1);
                        } else if (lua_type(L, -1) == LUA_TNUMBER) {
                            if (lua_isinteger(L, -1)) {
                                payload[key] = static_cast<int64_t>(lua_tointeger(L, -1));
                            } else {
                                payload[key] = lua_tonumber(L, -1);
                            }
                        } else if (lua_type(L, -1) == LUA_TBOOLEAN) {
                            payload[key] = lua_toboolean(L, -1) != 0;
                        }
                        lua_pop(L, 1);
                    }
                    LuaEventPayload lp;
                    lp.payload = std::move(payload);
                    bus->publish(std::string(event_name), std::move(lp));
                } else {
                    bus->publish(std::string(event_name));
                }
            }
            return 0;
        }

        static int lua_eventbus_unsubscribe(lua_State *L)
        {
            auto *bus = static_cast<HostEventBus *>(luaL_checkudata(L, 1, "yuan.HostEventBus"));
            auto token = static_cast<HostEventSubscription>(lua_tointeger(L, 2));
            if (bus) {
                lua_pushboolean(L, bus->unsubscribe(token) ? 1 : 0);
            } else {
                lua_pushboolean(L, 0);
            }
            return 1;
        }

        static const luaL_Reg eventbus_methods[] = {
            { "subscribe", lua_eventbus_subscribe },
            { "publish", lua_eventbus_publish },
            { "unsubscribe", lua_eventbus_unsubscribe },
            { nullptr, nullptr }
        };

        // ---- Scheduler binding ----

        static int lua_scheduler_schedule_after(lua_State *L)
        {
            auto *sched = static_cast<HostScheduler *>(luaL_checkudata(L, 1, "yuan.HostScheduler"));
            auto ms = static_cast<int>(luaL_checkinteger(L, 2));
            luaL_checktype(L, 3, LUA_TFUNCTION);
            const char *name = luaL_optstring(L, 4, "");

            lua_pushvalue(L, 3);
            int ref = luaL_ref(L, LUA_REGISTRYINDEX);

            auto *ctx_info = get_ctx_info(L);

            LuaSchedulerCallback cb;
            cb.L = L;
            cb.ref = ref;
            cb.plugin_name = ctx_info ? ctx_info->plugin_name : "";
            cb.max_instructions_per_call = ctx_info ? ctx_info->max_instructions_per_call : 0;
            cb.lua_mutex = ctx_info ? ctx_info->lua_mutex : nullptr;

            int cb_id;
            {
                std::lock_guard<std::mutex> lock(g_callbacks_mutex);
                cb_id = g_next_callback_id++;
                g_scheduler_callbacks[cb_id] = std::move(cb);
            }

            if (sched) {
                auto id = sched->schedule_after(std::chrono::milliseconds(ms),
                                                [cb_id]() {
                                                    LuaSchedulerCallback scb;
                                                    {
                                                        std::lock_guard<std::mutex> lock(g_callbacks_mutex);
                                                        auto it = g_scheduler_callbacks.find(cb_id);
                                                        if (it == g_scheduler_callbacks.end()) return;
                                                        scb = it->second;
                                                    }

                                                    if (!scb.lua_mutex) return;

                                                    std::lock_guard<std::recursive_mutex> lua_lock(*scb.lua_mutex);
                                                    lua_State *ls = scb.L;
                                                    lua_rawgeti(ls, LUA_REGISTRYINDEX, scb.ref);

                                                    set_callback_hook(ls, scb.max_instructions_per_call);
                                                    if (lua_pcall(ls, 0, 0, 0) != LUA_OK) {
                                                        const char *err = lua_tostring(ls, -1);
                                                        LOG_ERROR("lua scheduler callback error: {}", err ? err : "unknown");
                                                        lua_pop(ls, 1);
                                                    }
                                                    clear_callback_hook(ls);
                                                },
                                                name);

                auto *guard = ctx_info ? ctx_info->resource_guard : nullptr;
                if (guard) {
                    guard->track(cb.plugin_name, PluginResourceType::scheduler_task,
                                 [sched, id]() {
                                     if (sched) sched->cancel(id);
                                 },
                                 "task:" + std::string(name));
                }

                lua_pushinteger(L, static_cast<lua_Integer>(id));
            } else {
                lua_pushinteger(L, 0);
            }
            return 1;
        }

        static int lua_scheduler_schedule_interval(lua_State *L)
        {
            auto *sched = static_cast<HostScheduler *>(luaL_checkudata(L, 1, "yuan.HostScheduler"));
            auto ms = static_cast<int>(luaL_checkinteger(L, 2));
            luaL_checktype(L, 3, LUA_TFUNCTION);
            const char *name = luaL_optstring(L, 4, "");

            lua_pushvalue(L, 3);
            int ref = luaL_ref(L, LUA_REGISTRYINDEX);

            auto *ctx_info = get_ctx_info(L);

            LuaSchedulerCallback cb;
            cb.L = L;
            cb.ref = ref;
            cb.plugin_name = ctx_info ? ctx_info->plugin_name : "";
            cb.max_instructions_per_call = ctx_info ? ctx_info->max_instructions_per_call : 0;
            cb.lua_mutex = ctx_info ? ctx_info->lua_mutex : nullptr;

            int cb_id;
            {
                std::lock_guard<std::mutex> lock(g_callbacks_mutex);
                cb_id = g_next_callback_id++;
                g_scheduler_callbacks[cb_id] = std::move(cb);
            }

            if (sched) {
                auto id = sched->schedule_interval(std::chrono::milliseconds(ms),
                                                   [cb_id]() {
                                                       LuaSchedulerCallback scb;
                                                       {
                                                           std::lock_guard<std::mutex> lock(g_callbacks_mutex);
                                                           auto it = g_scheduler_callbacks.find(cb_id);
                                                           if (it == g_scheduler_callbacks.end()) return;
                                                           scb = it->second;
                                                       }

                                                       if (!scb.lua_mutex) return;

                                                       std::lock_guard<std::recursive_mutex> lua_lock(*scb.lua_mutex);
                                                       lua_State *ls = scb.L;
                                                       lua_rawgeti(ls, LUA_REGISTRYINDEX, scb.ref);

                                                       set_callback_hook(ls, scb.max_instructions_per_call);
                                                       if (lua_pcall(ls, 0, 0, 0) != LUA_OK) {
                                                           const char *err = lua_tostring(ls, -1);
                                                           LOG_ERROR("lua scheduler interval callback error: {}", err ? err : "unknown");
                                                           lua_pop(ls, 1);
                                                       }
                                                       clear_callback_hook(ls);
                                                   },
                                                   name);

                auto *guard = ctx_info ? ctx_info->resource_guard : nullptr;
                if (guard) {
                    guard->track(cb.plugin_name, PluginResourceType::scheduler_task,
                                 [sched, id]() {
                                     if (sched) sched->cancel(id);
                                 },
                                 "interval:" + std::string(name));
                }

                lua_pushinteger(L, static_cast<lua_Integer>(id));
            } else {
                lua_pushinteger(L, 0);
            }
            return 1;
        }

        static const luaL_Reg scheduler_methods[] = {
            { "schedule_after", lua_scheduler_schedule_after },
            { "schedule_interval", lua_scheduler_schedule_interval },
            { nullptr, nullptr }
        };

        static void register_userdata_type(lua_State *L, const char *meta_name, const luaL_Reg *methods)
        {
            luaL_newmetatable(L, meta_name);
            lua_pushvalue(L, -1);
            lua_setfield(L, -2, "__index");
            luaL_setfuncs(L, methods, 0);
            lua_pop(L, 1);
        }

        static void push_userdata(lua_State *L, void *ptr, const char *meta_name)
        {
            auto **ud = static_cast<void **>(lua_newuserdata(L, sizeof(void *)));
            *ud = ptr;
            luaL_setmetatable(L, meta_name);
        }

    } // namespace

    void push_json_to_lua(lua_State * L, const nlohmann::json & j)
    {
        push_json_value(L, j);
    }

    void lua_register_host_modules(lua_State * L, const PluginContext & context, size_t max_instructions_per_call, std::recursive_mutex * lua_mutex)
    {
        auto *ctx_info = new LuaCtxInfo();
        ctx_info->plugin_name = context.plugin_name;
        ctx_info->max_instructions_per_call = max_instructions_per_call;
        ctx_info->resource_guard = context.resource_guard;
        ctx_info->lua_mutex = lua_mutex;

        lua_pushstring(L, CTX_REGISTRY_KEY);
        lua_pushlightuserdata(L, static_cast<void *>(ctx_info));
        lua_rawset(L, LUA_REGISTRYINDEX);

        register_userdata_type(L, "yuan.HostLogger", logger_methods);
        register_userdata_type(L, "yuan.HostStorage", storage_methods);
        register_userdata_type(L, "yuan.HostEventBus", eventbus_methods);
        register_userdata_type(L, "yuan.HostScheduler", scheduler_methods);

        lua_createtable(L, 0, 8);

        if (context.logger) {
            push_userdata(L, context.logger, "yuan.HostLogger");
            lua_setfield(L, -2, "logger");
        }

        if (context.storage) {
            push_userdata(L, context.storage, "yuan.HostStorage");
            lua_setfield(L, -2, "storage");
        }

        if (context.event_bus) {
            push_userdata(L, context.event_bus, "yuan.HostEventBus");
            lua_setfield(L, -2, "event_bus");
        }

        if (context.scheduler) {
            push_userdata(L, context.scheduler, "yuan.HostScheduler");
            lua_setfield(L, -2, "scheduler");
        }

        lua_pushstring(L, context.app_name.c_str());
        lua_setfield(L, -2, "app_name");

        lua_pushstring(L, context.plugin_name.c_str());
        lua_setfield(L, -2, "plugin_name");

        if (context.config.loaded()) {
            auto *raw = context.config.raw();
            if (raw && raw->is_object()) {
                push_json_table(L, *raw);
                lua_setfield(L, -2, "config");
            }
        }
    }

    void cleanup_lua_plugin_callbacks(lua_State * L, const std::string & plugin_name)
    {
        {
            std::lock_guard<std::mutex> lock(g_callbacks_mutex);
            for (auto it = g_event_callbacks.begin(); it != g_event_callbacks.end();) {
                if (it->second.L == L && it->second.plugin_name == plugin_name) {
                    luaL_unref(L, LUA_REGISTRYINDEX, it->second.ref);
                    it = g_event_callbacks.erase(it);
                } else {
                    ++it;
                }
            }
            for (auto it = g_scheduler_callbacks.begin(); it != g_scheduler_callbacks.end();) {
                if (it->second.L == L && it->second.plugin_name == plugin_name) {
                    luaL_unref(L, LUA_REGISTRYINDEX, it->second.ref);
                    it = g_scheduler_callbacks.erase(it);
                } else {
                    ++it;
                }
            }
        }

        lua_pushstring(L, CTX_REGISTRY_KEY);
        lua_rawget(L, LUA_REGISTRYINDEX);
        if (lua_islightuserdata(L, -1)) {
            auto *info = static_cast<LuaCtxInfo *>(lua_touserdata(L, -1));
            delete info;
            lua_pop(L, 1);
            lua_pushstring(L, CTX_REGISTRY_KEY);
            lua_pushnil(L);
            lua_rawset(L, LUA_REGISTRYINDEX);
        } else {
            lua_pop(L, 1);
        }
    }

} // namespace yuan::plugin
