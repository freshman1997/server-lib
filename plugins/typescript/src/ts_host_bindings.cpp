#include "ts_host_bindings.h"
#include "plugin/host_logger.h"
#include "plugin/host_event_bus.h"
#include "plugin/host_scheduler.h"
#include "plugin/host_service_registry.h"
#include "plugin/host_http_interceptor.h"
#include "plugin/host_storage.h"
#include "plugin/host_resource_guard.h"
#include "plugin/plugin_context.h"
#include "logger.h"

#include "quickjs_lib.h"
#include <cassert>
#include "nlohmann/json.hpp"

#include <any>
#include <chrono>
#include <cstring>
#include <mutex>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::plugin
{

    namespace
    {

        static const char *CTX_REGISTRY_KEY = "_yuan_plugin_ts_ctx_info";

        struct TsCtxInfo
        {
            std::string plugin_name;
            size_t execution_timeout_ms;
            HostResourceGuard *resource_guard;
            std::recursive_mutex *js_mutex;
            TsInterruptData *interrupt_data;
        };

        class TsRegisteredService final : public PluginService
        {
        public:
            explicit TsRegisteredService(std::string value)
                : value_(std::move(value))
            {
            }

            const std::string &value() const
            {
                return value_;
            }

        private:
            std::string value_;
        };

        struct TsEventCallback
        {
            JSContext *ctx;
            JSValue callback;
            std::string plugin_name;
            size_t execution_timeout_ms;
            std::recursive_mutex *js_mutex;
            TsInterruptData *interrupt_data;
        };

        struct TsSchedulerCallback
        {
            JSContext *ctx;
            JSValue callback;
            std::string plugin_name;
            size_t execution_timeout_ms;
            std::recursive_mutex *js_mutex;
            TsInterruptData *interrupt_data;
        };

        struct TsEventPayload
        {
            std::any payload;
        };

        static std::unordered_map<int, TsEventCallback> g_ts_event_callbacks;
        static std::unordered_map<int, TsSchedulerCallback> g_ts_scheduler_callbacks;
        static int g_ts_next_callback_id = 1;
        static std::mutex g_ts_callbacks_mutex;

        static bool cleanup_event_callback_by_id(int cb_id)
        {
            JSContext *ctx = nullptr;
            JSValue callback = JS_UNDEFINED;
            std::recursive_mutex *js_mutex = nullptr;

            {
                std::lock_guard<std::mutex> lock(g_ts_callbacks_mutex);
                auto it = g_ts_event_callbacks.find(cb_id);
                if (it == g_ts_event_callbacks.end()) {
                    return false;
                }
                ctx = it->second.ctx;
                callback = it->second.callback;
                js_mutex = it->second.js_mutex;
                g_ts_event_callbacks.erase(it);
            }

            if (!ctx) {
                return false;
            }

            if (js_mutex) {
                std::lock_guard<std::recursive_mutex> js_lock(*js_mutex);
                JS_FreeValue(ctx, callback);
            } else {
                JS_FreeValue(ctx, callback);
            }
            return true;
        }

        static bool cleanup_scheduler_callback_by_id(int cb_id)
        {
            JSContext *ctx = nullptr;
            JSValue callback = JS_UNDEFINED;
            std::recursive_mutex *js_mutex = nullptr;

            {
                std::lock_guard<std::mutex> lock(g_ts_callbacks_mutex);
                auto it = g_ts_scheduler_callbacks.find(cb_id);
                if (it == g_ts_scheduler_callbacks.end()) {
                    return false;
                }
                ctx = it->second.ctx;
                callback = it->second.callback;
                js_mutex = it->second.js_mutex;
                g_ts_scheduler_callbacks.erase(it);
            }

            if (!ctx) {
                return false;
            }

            if (js_mutex) {
                std::lock_guard<std::recursive_mutex> js_lock(*js_mutex);
                JS_FreeValue(ctx, callback);
            } else {
                JS_FreeValue(ctx, callback);
            }
            return true;
        }

        static JSClassID js_logger_class_id = 0;
        static JSClassID js_storage_class_id = 0;
        static JSClassID js_eventbus_class_id = 0;
        static JSClassID js_scheduler_class_id = 0;
        static JSClassID js_service_registry_class_id = 0;
        static JSClassID js_http_interceptor_class_id = 0;
        static JSClassID js_ctx_info_class_id = 0;

        static TsCtxInfo *get_ctx_info(JSContext *ctx)
        {
            JSValue global = JS_GetGlobalObject(ctx);
            JSAtom atom = JS_NewAtom(ctx, CTX_REGISTRY_KEY);
            JSValue val = JS_GetProperty(ctx, global, atom);

            TsCtxInfo *info = nullptr;
            if (JS_IsObject(val)) {
                info = static_cast<TsCtxInfo *>(JS_GetOpaque(val, js_ctx_info_class_id));
            }

            JS_FreeValue(ctx, val);
            JS_FreeAtom(ctx, atom);
            JS_FreeValue(ctx, global);
            return info;
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

        static JSValue make_json_value(JSContext *ctx, const nlohmann::json &val);

        static JSValue make_json_object(JSContext *ctx, const nlohmann::json &j)
        {
            JSValue obj = JS_NewObject(ctx);
            for (auto it = j.begin(); it != j.end(); ++it) {
                JS_SetPropertyStr(ctx, obj, it.key().c_str(), make_json_value(ctx, it.value()));
            }
            return obj;
        }

        static JSValue make_json_array(JSContext *ctx, const nlohmann::json &j)
        {
            JSValue arr = JS_NewArray(ctx);
            for (size_t i = 0; i < j.size(); ++i) {
                JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), make_json_value(ctx, j[i]));
            }
            return arr;
        }

        static JSValue make_json_value(JSContext *ctx, const nlohmann::json &val)
        {
            if (val.is_null()) {
                return JS_NULL;
            } else if (val.is_boolean()) {
                return JS_NewBool(ctx, val.get<bool>());
            } else if (val.is_number_integer()) {
                return JS_NewInt64(ctx, val.get<int64_t>());
            } else if (val.is_number_float()) {
                return JS_NewFloat64(ctx, val.get<double>());
            } else if (val.is_string()) {
                return JS_NewString(ctx, val.get_ref<const std::string &>().c_str());
            } else if (val.is_array()) {
                return make_json_array(ctx, val);
            } else if (val.is_object()) {
                return make_json_object(ctx, val);
            }
            return JS_NULL;
        }

        static void set_callback_deadline(TsInterruptData *data, size_t timeout_ms)
        {
            data->timeout_triggered.store(false, std::memory_order_relaxed);
            if (timeout_ms > 0) {
                data->deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
            } else {
                data->deadline = std::chrono::steady_clock::time_point::max();
            }
        }

        static void clear_callback_deadline(TsInterruptData *data)
        {
            data->deadline = std::chrono::steady_clock::time_point::max();
        }

        // ---- Logger binding ----

        static JSValue js_logger_log(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            if (argc < 2) {
                return JS_ThrowTypeError(ctx, "logger.log requires 2 arguments (level, message)");
            }
            auto *logger = static_cast<HostLogger *>(JS_GetOpaque(this_val, js_logger_class_id));
            const char *level_str = JS_ToCString(ctx, argv[0]);
            const char *msg = JS_ToCString(ctx, argv[1]);
            if (logger && level_str && msg) {
                logger->log(parse_log_level(level_str), "", 0, "", std::string_view(msg));
            }
            if (level_str)
                JS_FreeCString(ctx, level_str);
            if (msg)
                JS_FreeCString(ctx, msg);
            return JS_UNDEFINED;
        }

        static JSValue js_logger_info(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            if (argc < 1) {
                return JS_ThrowTypeError(ctx, "logger.info requires 1 argument (message)");
            }
            auto *logger = static_cast<HostLogger *>(JS_GetOpaque(this_val, js_logger_class_id));
            const char *msg = JS_ToCString(ctx, argv[0]);
            if (logger && msg) {
                logger->log(HostLogLevel::info, "", 0, "", std::string_view(msg));
            }
            if (msg)
                JS_FreeCString(ctx, msg);
            return JS_UNDEFINED;
        }

        static JSValue js_logger_warn(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            if (argc < 1) {
                return JS_ThrowTypeError(ctx, "logger.warn requires 1 argument (message)");
            }
            auto *logger = static_cast<HostLogger *>(JS_GetOpaque(this_val, js_logger_class_id));
            const char *msg = JS_ToCString(ctx, argv[0]);
            if (logger && msg) {
                logger->log(HostLogLevel::warn, "", 0, "", std::string_view(msg));
            }
            if (msg)
                JS_FreeCString(ctx, msg);
            return JS_UNDEFINED;
        }

        static JSValue js_logger_error(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            if (argc < 1) {
                return JS_ThrowTypeError(ctx, "logger.error requires 1 argument (message)");
            }
            auto *logger = static_cast<HostLogger *>(JS_GetOpaque(this_val, js_logger_class_id));
            const char *msg = JS_ToCString(ctx, argv[0]);
            if (logger && msg) {
                logger->log(HostLogLevel::error, "", 0, "", std::string_view(msg));
            }
            if (msg)
                JS_FreeCString(ctx, msg);
            return JS_UNDEFINED;
        }

        static JSValue js_logger_debug(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            if (argc < 1) {
                return JS_ThrowTypeError(ctx, "logger.debug requires 1 argument (message)");
            }
            auto *logger = static_cast<HostLogger *>(JS_GetOpaque(this_val, js_logger_class_id));
            const char *msg = JS_ToCString(ctx, argv[0]);
            if (logger && msg) {
                logger->log(HostLogLevel::debug, "", 0, "", std::string_view(msg));
            }
            if (msg)
                JS_FreeCString(ctx, msg);
            return JS_UNDEFINED;
        }

        static JSValue js_logger_trace(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            if (argc < 1) {
                return JS_ThrowTypeError(ctx, "logger.trace requires 1 argument (message)");
            }
            auto *logger = static_cast<HostLogger *>(JS_GetOpaque(this_val, js_logger_class_id));
            const char *msg = JS_ToCString(ctx, argv[0]);
            if (logger && msg) {
                logger->log(HostLogLevel::trace, "", 0, "", std::string_view(msg));
            }
            if (msg)
                JS_FreeCString(ctx, msg);
            return JS_UNDEFINED;
        }

        static JSValue js_logger_fatal(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            if (argc < 1) {
                return JS_ThrowTypeError(ctx, "logger.fatal requires 1 argument (message)");
            }
            auto *logger = static_cast<HostLogger *>(JS_GetOpaque(this_val, js_logger_class_id));
            const char *msg = JS_ToCString(ctx, argv[0]);
            if (logger && msg) {
                logger->log(HostLogLevel::fatal, "", 0, "", std::string_view(msg));
            }
            if (msg)
                JS_FreeCString(ctx, msg);
            return JS_UNDEFINED;
        }

        static const JSCFunctionListEntry js_logger_proto_funcs[] = {
            JS_CFUNC_DEF("log", 2, js_logger_log),
            JS_CFUNC_DEF("info", 1, js_logger_info),
            JS_CFUNC_DEF("warn", 1, js_logger_warn),
            JS_CFUNC_DEF("error", 1, js_logger_error),
            JS_CFUNC_DEF("debug", 1, js_logger_debug),
            JS_CFUNC_DEF("trace", 1, js_logger_trace),
            JS_CFUNC_DEF("fatal", 1, js_logger_fatal),
        };

        static void js_logger_finalizer(JSRuntime *rt, JSValue val)
        {
        }

        // ---- Storage binding ----

        static JSValue js_storage_set(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            if (argc < 2) {
                return JS_ThrowTypeError(ctx, "storage.set requires 2 arguments (key, value)");
            }
            auto *storage = static_cast<HostStorage *>(JS_GetOpaque(this_val, js_storage_class_id));
            const char *key = JS_ToCString(ctx, argv[0]);
            const char *value = JS_ToCString(ctx, argv[1]);
            bool result = false;
            if (storage && key && value) {
                result = storage->set(key, value);
            }
            if (key)
                JS_FreeCString(ctx, key);
            if (value)
                JS_FreeCString(ctx, value);
            return JS_NewBool(ctx, result);
        }

        static JSValue js_storage_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            if (argc < 1) {
                return JS_ThrowTypeError(ctx, "storage.get requires 1 argument (key)");
            }
            auto *storage = static_cast<HostStorage *>(JS_GetOpaque(this_val, js_storage_class_id));
            const char *key = JS_ToCString(ctx, argv[0]);
            if (storage && key) {
                auto val = storage->get(key);
                JS_FreeCString(ctx, key);
                if (val) {
                    return JS_NewString(ctx, val->c_str());
                }
            } else {
                if (key)
                    JS_FreeCString(ctx, key);
            }
            return JS_NULL;
        }

        static JSValue js_storage_del(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            if (argc < 1) {
                return JS_ThrowTypeError(ctx, "storage.del requires 1 argument (key)");
            }
            auto *storage = static_cast<HostStorage *>(JS_GetOpaque(this_val, js_storage_class_id));
            const char *key = JS_ToCString(ctx, argv[0]);
            bool result = false;
            if (storage && key) {
                result = storage->del(key);
            }
            if (key)
                JS_FreeCString(ctx, key);
            return JS_NewBool(ctx, result);
        }

        static JSValue js_storage_exists(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            if (argc < 1) {
                return JS_ThrowTypeError(ctx, "storage.exists requires 1 argument (key)");
            }
            auto *storage = static_cast<HostStorage *>(JS_GetOpaque(this_val, js_storage_class_id));
            const char *key = JS_ToCString(ctx, argv[0]);
            bool result = false;
            if (storage && key) {
                result = storage->exists(key);
            }
            if (key)
                JS_FreeCString(ctx, key);
            return JS_NewBool(ctx, result);
        }

        static JSValue js_storage_hset(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            if (argc < 3) {
                return JS_ThrowTypeError(ctx, "storage.hset requires 3 arguments (key, field, value)");
            }
            auto *storage = static_cast<HostStorage *>(JS_GetOpaque(this_val, js_storage_class_id));
            const char *key = JS_ToCString(ctx, argv[0]);
            const char *field = JS_ToCString(ctx, argv[1]);
            const char *value = JS_ToCString(ctx, argv[2]);
            bool result = false;
            if (storage && key && field && value) {
                result = storage->hset(key, field, value);
            }
            if (key)
                JS_FreeCString(ctx, key);
            if (field)
                JS_FreeCString(ctx, field);
            if (value)
                JS_FreeCString(ctx, value);
            return JS_NewBool(ctx, result);
        }

        static JSValue js_storage_hget(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            if (argc < 2) {
                return JS_ThrowTypeError(ctx, "storage.hget requires 2 arguments (key, field)");
            }
            auto *storage = static_cast<HostStorage *>(JS_GetOpaque(this_val, js_storage_class_id));
            const char *key = JS_ToCString(ctx, argv[0]);
            const char *field = JS_ToCString(ctx, argv[1]);
            if (storage && key && field) {
                auto val = storage->hget(key, field);
                JS_FreeCString(ctx, key);
                JS_FreeCString(ctx, field);
                if (val) {
                    return JS_NewString(ctx, val->c_str());
                }
            } else {
                if (key)
                    JS_FreeCString(ctx, key);
                if (field)
                    JS_FreeCString(ctx, field);
            }
            return JS_NULL;
        }

        static JSValue js_storage_hdel(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            if (argc < 2) {
                return JS_ThrowTypeError(ctx, "storage.hdel requires 2 arguments (key, field)");
            }
            auto *storage = static_cast<HostStorage *>(JS_GetOpaque(this_val, js_storage_class_id));
            const char *key = JS_ToCString(ctx, argv[0]);
            const char *field = JS_ToCString(ctx, argv[1]);
            bool result = false;
            if (storage && key && field) {
                result = storage->hdel(key, field);
            }
            if (key)
                JS_FreeCString(ctx, key);
            if (field)
                JS_FreeCString(ctx, field);
            return JS_NewBool(ctx, result);
        }

        static JSValue js_storage_hgetall(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            if (argc < 1) {
                return JS_ThrowTypeError(ctx, "storage.hgetall requires 1 argument (key)");
            }
            auto *storage = static_cast<HostStorage *>(JS_GetOpaque(this_val, js_storage_class_id));
            const char *key = JS_ToCString(ctx, argv[0]);
            JSValue obj = JS_NewObject(ctx);
            if (storage && key) {
                auto map = storage->hgetall(key);
                for (auto & [
                                k,
                                v
                            ] : map) {
                    JS_SetPropertyStr(ctx, obj, k.c_str(), JS_NewString(ctx, v.c_str()));
                }
            }
            if (key)
                JS_FreeCString(ctx, key);
            return obj;
        }

        static JSValue js_storage_is_available(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            auto *storage = static_cast<HostStorage *>(JS_GetOpaque(this_val, js_storage_class_id));
            return JS_NewBool(ctx, storage ? storage->is_available() : false);
        }

        static const JSCFunctionListEntry js_storage_proto_funcs[] = {
            JS_CFUNC_DEF("set", 2, js_storage_set),
            JS_CFUNC_DEF("get", 1, js_storage_get),
            JS_CFUNC_DEF("del", 1, js_storage_del),
            JS_CFUNC_DEF("exists", 1, js_storage_exists),
            JS_CFUNC_DEF("hset", 3, js_storage_hset),
            JS_CFUNC_DEF("hget", 2, js_storage_hget),
            JS_CFUNC_DEF("hdel", 2, js_storage_hdel),
            JS_CFUNC_DEF("hgetall", 1, js_storage_hgetall),
            JS_CFUNC_DEF("is_available", 0, js_storage_is_available),
        };

        static void js_storage_finalizer(JSRuntime *rt, JSValue val)
        {
        }

        // ---- EventBus binding ----

        static JSValue js_eventbus_subscribe(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            if (argc < 2) {
                return JS_ThrowTypeError(ctx, "eventBus.subscribe requires 2 arguments (eventName, callback)");
            }
            if (!JS_IsFunction(ctx, argv[1])) {
                return JS_ThrowTypeError(ctx, "eventBus.subscribe: second argument must be a function");
            }
            auto *bus = static_cast<HostEventBus *>(JS_GetOpaque(this_val, js_eventbus_class_id));
            const char *event_name = JS_ToCString(ctx, argv[0]);

            if (!bus || !event_name) {
                if (event_name)
                    JS_FreeCString(ctx, event_name);
                return JS_UNDEFINED;
            }

            auto *ctx_info = get_ctx_info(ctx);

            TsEventCallback cb;
            cb.ctx = ctx;
            cb.callback = JS_DupValue(ctx, argv[1]);
            cb.plugin_name = ctx_info ? ctx_info->plugin_name : "";
            cb.execution_timeout_ms = ctx_info ? ctx_info->execution_timeout_ms : 0;
            cb.js_mutex = ctx_info ? ctx_info->js_mutex : nullptr;
            cb.interrupt_data = ctx_info ? ctx_info->interrupt_data : nullptr;
            const auto tracked_plugin_name = cb.plugin_name;

            int cb_id;
            {
                std::lock_guard<std::mutex> lock(g_ts_callbacks_mutex);
                cb_id = g_ts_next_callback_id++;
                g_ts_event_callbacks[cb_id] = std::move(cb);
            }

            auto token = bus->subscribe(event_name, [cb_id](const HostEvent &event) {
                TsEventCallback ecb;
                {
                    std::lock_guard<std::mutex> lock(g_ts_callbacks_mutex);
                    auto it = g_ts_event_callbacks.find(cb_id);
                    if (it == g_ts_event_callbacks.end()) return;
                    ecb = it->second;
                }

                if (!ecb.js_mutex) return;

                std::lock_guard<std::recursive_mutex> js_lock(*ecb.js_mutex);
                JSContext *js_ctx = ecb.ctx;

                if (ecb.interrupt_data) {
                    set_callback_deadline(ecb.interrupt_data, ecb.execution_timeout_ms);
                }

                JSValue event_obj = JS_NewObject(js_ctx);
                JS_SetPropertyStr(js_ctx, event_obj, "name", JS_NewString(js_ctx, event.name.c_str()));

                if (event.payload.has_value()) {
                    auto *payload_ptr = std::any_cast<TsEventPayload>(&event.payload);
                    if (payload_ptr && payload_ptr->payload.has_value()) {
                        auto *json_ptr = std::any_cast<nlohmann::json>(&payload_ptr->payload);
                        if (json_ptr) {
                            JS_SetPropertyStr(js_ctx, event_obj, "payload", make_json_object(js_ctx, *json_ptr));
                        } else {
                            JS_SetPropertyStr(js_ctx, event_obj, "payload", JS_NULL);
                        }
                    } else {
                        JS_SetPropertyStr(js_ctx, event_obj, "payload", JS_NULL);
                    }
                } else {
                    JS_SetPropertyStr(js_ctx, event_obj, "payload", JS_NULL);
                }

                JSValue argv[] = {event_obj};
                JSValue ret = JS_Call(js_ctx, ecb.callback, JS_UNDEFINED, 1, argv);

                if (ecb.interrupt_data) {
                    clear_callback_deadline(ecb.interrupt_data);
                }

                if (ecb.interrupt_data && ecb.interrupt_data->timeout_triggered.load(std::memory_order_relaxed)) {
                    LOG_ERROR("ts event callback timed out for plugin '{}'", ecb.plugin_name);
                    if (JS_IsException(ret)) {
                        JSValue exception = JS_GetException(js_ctx);
                        JS_FreeValue(js_ctx, exception);
                    }
                } else if (JS_IsException(ret)) {
                    JSValue exception = JS_GetException(js_ctx);
                    const char *err = JS_ToCString(js_ctx, exception);
                    LOG_ERROR("ts event callback error: {}", err ? err : "unknown");
                    JS_FreeCString(js_ctx, err);
                    JS_FreeValue(js_ctx, exception);
                }

                JS_FreeValue(js_ctx, ret);
                JS_FreeValue(js_ctx, event_obj);
            });

            auto *guard = ctx_info ? ctx_info->resource_guard : nullptr;
            if (guard) {
                guard->track(tracked_plugin_name,
                             PluginResourceType::callback,
                             [cb_id]() {
                                 (void)cleanup_event_callback_by_id(cb_id);
                             },
                             "callback:event:" + std::string(event_name));
                guard->track(tracked_plugin_name, PluginResourceType::event_subscription,
                             [bus, token]() {
                                  if (bus) bus->unsubscribe(token);
                              },
                              "event:" + std::string(event_name));
            }

            JS_FreeCString(ctx, event_name);
            return JS_UNDEFINED;
        }

        static JSValue js_eventbus_publish(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            if (argc < 1) {
                return JS_ThrowTypeError(ctx, "eventBus.publish requires at least 1 argument (eventName)");
            }
            auto *bus = static_cast<HostEventBus *>(JS_GetOpaque(this_val, js_eventbus_class_id));
            const char *event_name = JS_ToCString(ctx, argv[0]);

            if (bus && event_name) {
                if (argc > 1 && JS_IsObject(argv[1])) {
                    nlohmann::json payload;
                    JSPropertyEnum *ptab = nullptr;
                    uint32_t plen = 0;
                    if (JS_GetOwnPropertyNames(ctx, &ptab, &plen, argv[1],
                                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
                        for (uint32_t i = 0; i < plen; i++) {
                            JSValue key_val = JS_AtomToValue(ctx, ptab[i].atom);
                            JSValue prop_val = JS_GetProperty(ctx, argv[1], ptab[i].atom);
                            const char *key_str = JS_ToCString(ctx, key_val);
                            if (key_str) {
                                if (JS_IsString(prop_val)) {
                                    const char *val_str = JS_ToCString(ctx, prop_val);
                                    payload[key_str] = val_str;
                                    JS_FreeCString(ctx, val_str);
                                } else if (JS_IsNumber(prop_val)) {
                                    double d = 0;
                                    JS_ToFloat64(ctx, &d, prop_val);
                                    payload[key_str] = d;
                                } else if (JS_IsBool(prop_val)) {
                                    payload[key_str] = JS_ToBool(ctx, prop_val) != 0;
                                }
                            }
                            JS_FreeCString(ctx, key_str);
                            JS_FreeValue(ctx, key_val);
                            JS_FreeValue(ctx, prop_val);
                        }
                        JS_FreePropertyEnum(ctx, ptab, plen);
                    }
                    TsEventPayload lp;
                    lp.payload = std::move(payload);
                    bus->publish(std::string(event_name), std::move(lp));
                } else {
                    bus->publish(std::string(event_name));
                }
            }

            if (event_name)
                JS_FreeCString(ctx, event_name);
            return JS_UNDEFINED;
        }

        static JSValue js_eventbus_unsubscribe(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            if (argc < 1) {
                return JS_ThrowTypeError(ctx, "eventBus.unsubscribe requires 1 argument (token)");
            }
            auto *bus = static_cast<HostEventBus *>(JS_GetOpaque(this_val, js_eventbus_class_id));
            int64_t token_val = 0;
            JS_ToInt64(ctx, &token_val, argv[0]);
            auto token = static_cast<HostEventSubscription>(token_val);
            bool result = false;
            if (bus) {
                result = bus->unsubscribe(token);
            }
            return JS_NewBool(ctx, result);
        }

        static const JSCFunctionListEntry js_eventbus_proto_funcs[] = {
            JS_CFUNC_DEF("subscribe", 2, js_eventbus_subscribe),
            JS_CFUNC_DEF("publish", 2, js_eventbus_publish),
            JS_CFUNC_DEF("unsubscribe", 1, js_eventbus_unsubscribe),
        };

        static void js_eventbus_finalizer(JSRuntime *rt, JSValue val)
        {
        }

        // ---- Scheduler binding ----

        static JSValue js_scheduler_schedule_after(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            if (argc < 2) {
                return JS_ThrowTypeError(ctx, "scheduler.schedule_after requires 2 arguments (ms, callback)");
            }
            if (!JS_IsFunction(ctx, argv[1])) {
                return JS_ThrowTypeError(ctx, "scheduler.schedule_after: second argument must be a function");
            }
            auto *sched = static_cast<HostScheduler *>(JS_GetOpaque(this_val, js_scheduler_class_id));
            int32_t ms = 0;
            JS_ToInt32(ctx, &ms, argv[0]);
            const char *name = nullptr;
            if (argc > 2 && JS_IsString(argv[2])) {
                name = JS_ToCString(ctx, argv[2]);
            }

            if (!sched) {
                if (name)
                    JS_FreeCString(ctx, name);
                return JS_NewInt32(ctx, 0);
            }

            auto *ctx_info = get_ctx_info(ctx);

            TsSchedulerCallback cb;
            cb.ctx = ctx;
            cb.callback = JS_DupValue(ctx, argv[1]);
            cb.plugin_name = ctx_info ? ctx_info->plugin_name : "";
            cb.execution_timeout_ms = ctx_info ? ctx_info->execution_timeout_ms : 0;
            cb.js_mutex = ctx_info ? ctx_info->js_mutex : nullptr;
            cb.interrupt_data = ctx_info ? ctx_info->interrupt_data : nullptr;
            const auto tracked_plugin_name = cb.plugin_name;

            int cb_id;
            {
                std::lock_guard<std::mutex> lock(g_ts_callbacks_mutex);
                cb_id = g_ts_next_callback_id++;
                g_ts_scheduler_callbacks[cb_id] = std::move(cb);
            }

            std::string name_str(name ? name : "");

            auto id = sched->schedule_after(std::chrono::milliseconds(ms),
                                            [cb_id]() {
                                                TsSchedulerCallback scb;
                                                {
                                                    std::lock_guard<std::mutex> lock(g_ts_callbacks_mutex);
                                                    auto it = g_ts_scheduler_callbacks.find(cb_id);
                                                    if (it == g_ts_scheduler_callbacks.end()) return;
                                                    scb = it->second;
                                                }

                                                if (!scb.js_mutex) return;

                                                std::lock_guard<std::recursive_mutex> js_lock(*scb.js_mutex);
                                                JSContext *js_ctx = scb.ctx;

                                                if (scb.interrupt_data) {
                                                    set_callback_deadline(scb.interrupt_data, scb.execution_timeout_ms);
                                                }

                                                JSValue ret = JS_Call(js_ctx, scb.callback, JS_UNDEFINED, 0, nullptr);

                                                if (scb.interrupt_data) {
                                                    clear_callback_deadline(scb.interrupt_data);
                                                }

                                                if (scb.interrupt_data && scb.interrupt_data->timeout_triggered.load(std::memory_order_relaxed)) {
                                                    LOG_ERROR("ts scheduler callback timed out for plugin '{}'", scb.plugin_name);
                                                    if (JS_IsException(ret)) {
                                                        JSValue exception = JS_GetException(js_ctx);
                                                        JS_FreeValue(js_ctx, exception);
                                                    }
                                                } else if (JS_IsException(ret)) {
                                                    JSValue exception = JS_GetException(js_ctx);
                                                    const char *err = JS_ToCString(js_ctx, exception);
                                                    LOG_ERROR("ts scheduler callback error: {}", err ? err : "unknown");
                                                    JS_FreeCString(js_ctx, err);
                                                    JS_FreeValue(js_ctx, exception);
                                                }

                                                JS_FreeValue(js_ctx, ret);
                                            },
                                            name_str);

            auto *guard = ctx_info ? ctx_info->resource_guard : nullptr;
            if (guard) {
                guard->track(tracked_plugin_name,
                             PluginResourceType::callback,
                             [cb_id]() {
                                 (void)cleanup_scheduler_callback_by_id(cb_id);
                             },
                             "callback:scheduler:after:" + name_str);
                guard->track(tracked_plugin_name, PluginResourceType::scheduler_task,
                             [sched, id]() {
                                  if (sched) sched->cancel(id);
                              },
                              "task:" + name_str);
            }

            if (name)
                JS_FreeCString(ctx, name);
            return JS_NewInt64(ctx, static_cast<int64_t>(id));
        }

        static JSValue js_scheduler_schedule_interval(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            if (argc < 2) {
                return JS_ThrowTypeError(ctx, "scheduler.schedule_interval requires 2 arguments (ms, callback)");
            }
            if (!JS_IsFunction(ctx, argv[1])) {
                return JS_ThrowTypeError(ctx, "scheduler.schedule_interval: second argument must be a function");
            }
            auto *sched = static_cast<HostScheduler *>(JS_GetOpaque(this_val, js_scheduler_class_id));
            int32_t ms = 0;
            JS_ToInt32(ctx, &ms, argv[0]);
            const char *name = nullptr;
            if (argc > 2 && JS_IsString(argv[2])) {
                name = JS_ToCString(ctx, argv[2]);
            }

            if (!sched) {
                if (name)
                    JS_FreeCString(ctx, name);
                return JS_NewInt32(ctx, 0);
            }

            auto *ctx_info = get_ctx_info(ctx);

            TsSchedulerCallback cb;
            cb.ctx = ctx;
            cb.callback = JS_DupValue(ctx, argv[1]);
            cb.plugin_name = ctx_info ? ctx_info->plugin_name : "";
            cb.execution_timeout_ms = ctx_info ? ctx_info->execution_timeout_ms : 0;
            cb.js_mutex = ctx_info ? ctx_info->js_mutex : nullptr;
            cb.interrupt_data = ctx_info ? ctx_info->interrupt_data : nullptr;
            const auto tracked_plugin_name = cb.plugin_name;

            int cb_id;
            {
                std::lock_guard<std::mutex> lock(g_ts_callbacks_mutex);
                cb_id = g_ts_next_callback_id++;
                g_ts_scheduler_callbacks[cb_id] = std::move(cb);
            }

            std::string name_str(name ? name : "");

            auto id = sched->schedule_interval(std::chrono::milliseconds(ms),
                                               [cb_id]() {
                                                   TsSchedulerCallback scb;
                                                   {
                                                       std::lock_guard<std::mutex> lock(g_ts_callbacks_mutex);
                                                       auto it = g_ts_scheduler_callbacks.find(cb_id);
                                                       if (it == g_ts_scheduler_callbacks.end()) return;
                                                       scb = it->second;
                                                   }

                                                   if (!scb.js_mutex) return;

                                                   std::lock_guard<std::recursive_mutex> js_lock(*scb.js_mutex);
                                                   JSContext *js_ctx = scb.ctx;

                                                   if (scb.interrupt_data) {
                                                       set_callback_deadline(scb.interrupt_data, scb.execution_timeout_ms);
                                                   }

                                                   JSValue ret = JS_Call(js_ctx, scb.callback, JS_UNDEFINED, 0, nullptr);

                                                   if (scb.interrupt_data) {
                                                       clear_callback_deadline(scb.interrupt_data);
                                                   }

                                                   if (scb.interrupt_data && scb.interrupt_data->timeout_triggered.load(std::memory_order_relaxed)) {
                                                       LOG_ERROR("ts scheduler interval callback timed out for plugin '{}'", scb.plugin_name);
                                                       if (JS_IsException(ret)) {
                                                           JSValue exception = JS_GetException(js_ctx);
                                                           JS_FreeValue(js_ctx, exception);
                                                       }
                                                   } else if (JS_IsException(ret)) {
                                                       JSValue exception = JS_GetException(js_ctx);
                                                       const char *err = JS_ToCString(js_ctx, exception);
                                                       LOG_ERROR("ts scheduler interval callback error: {}", err ? err : "unknown");
                                                       JS_FreeCString(js_ctx, err);
                                                       JS_FreeValue(js_ctx, exception);
                                                   }

                                                   JS_FreeValue(js_ctx, ret);
                                               },
                                               name_str);

            auto *guard = ctx_info ? ctx_info->resource_guard : nullptr;
            if (guard) {
                guard->track(tracked_plugin_name,
                             PluginResourceType::callback,
                             [cb_id]() {
                                 (void)cleanup_scheduler_callback_by_id(cb_id);
                             },
                             "callback:scheduler:interval:" + name_str);
                guard->track(tracked_plugin_name, PluginResourceType::scheduler_task,
                             [sched, id]() {
                                  if (sched) sched->cancel(id);
                              },
                              "interval:" + name_str);
            }

            if (name)
                JS_FreeCString(ctx, name);
            return JS_NewInt64(ctx, static_cast<int64_t>(id));
        }

        static const JSCFunctionListEntry js_scheduler_proto_funcs[] = {
            JS_CFUNC_DEF("schedule_after", 2, js_scheduler_schedule_after),
            JS_CFUNC_DEF("schedule_interval", 2, js_scheduler_schedule_interval),
        };

        static void js_scheduler_finalizer(JSRuntime *rt, JSValue val)
        {
        }

        // ---- ServiceRegistry binding ----

        static JSValue make_service_descriptor(JSContext *ctx, const PluginServiceDescriptor &descriptor)
        {
            JSValue obj = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, descriptor.name.c_str()));
            JS_SetPropertyStr(ctx, obj, "pluginName", JS_NewString(ctx, descriptor.plugin_name.c_str()));
            JS_SetPropertyStr(ctx, obj, "typeName", JS_NewString(ctx, descriptor.type_name.c_str()));
            JS_SetPropertyStr(ctx, obj, "contractId", JS_NewString(ctx, descriptor.contract_id.c_str()));
            JS_SetPropertyStr(ctx, obj, "contractVersion", JS_NewInt32(ctx, descriptor.contract_version));
            JS_SetPropertyStr(ctx, obj, "managedLifecycle", JS_NewBool(ctx, descriptor.managed_lifecycle));
            return obj;
        }

        static JSValue js_service_registry_register(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            if (argc < 1) {
                return JS_ThrowTypeError(ctx, "serviceRegistry.register requires at least 1 argument");
            }
            auto *registry = static_cast<HostServiceRegistry *>(JS_GetOpaque(this_val, js_service_registry_class_id));
            auto *ctx_info = get_ctx_info(ctx);
            if (!registry || !ctx_info || ctx_info->plugin_name.empty()) {
                return JS_NewBool(ctx, false);
            }

            const char *name = JS_ToCString(ctx, argv[0]);
            const char *contract_id = argc > 1 ? JS_ToCString(ctx, argv[1]) : nullptr;
            int32_t contract_version = 1;
            if (argc > 2) {
                JS_ToInt32(ctx, &contract_version, argv[2]);
            }
            const char *type_name = argc > 3 ? JS_ToCString(ctx, argv[3]) : nullptr;
            const char *value = argc > 4 ? JS_ToCString(ctx, argv[4]) : nullptr;

            PluginServiceDescriptor descriptor;
            descriptor.name = name ? name : "";
            descriptor.plugin_name = ctx_info->plugin_name;
            descriptor.type_name = type_name ? type_name : "script.ts.service";
            descriptor.contract_id = contract_id ? contract_id : descriptor.name;
            descriptor.contract_version = contract_version <= 0 ? 1 : contract_version;
            descriptor.managed_lifecycle = true;

            auto service = std::make_shared<TsRegisteredService>(value ? value : "");
            const bool ok = registry->register_service(
                ctx_info->plugin_name, descriptor, make_plugin_service(std::move(service)));

            if (ok && ctx_info->resource_guard) {
                auto *reg = registry;
                const auto plugin_name = ctx_info->plugin_name;
                ctx_info->resource_guard->track(
                    plugin_name,
                    PluginResourceType::service_registration,
                    [reg, plugin_name]() {
                        if (reg) {
                            reg->unregister_plugin_services(plugin_name);
                        }
                    },
                    "service:" + descriptor.name);
            }

            if (name) JS_FreeCString(ctx, name);
            if (contract_id) JS_FreeCString(ctx, contract_id);
            if (type_name) JS_FreeCString(ctx, type_name);
            if (value) JS_FreeCString(ctx, value);
            return JS_NewBool(ctx, ok);
        }

        static JSValue js_service_registry_has(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            if (argc < 1) {
                return JS_ThrowTypeError(ctx, "serviceRegistry.has requires 1 argument");
            }
            auto *registry = static_cast<HostServiceRegistry *>(JS_GetOpaque(this_val, js_service_registry_class_id));
            const char *name = JS_ToCString(ctx, argv[0]);
            const bool ok = registry && name && registry->has_service(name);
            if (name) JS_FreeCString(ctx, name);
            return JS_NewBool(ctx, ok);
        }

        static JSValue js_service_registry_describe(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            if (argc < 1) {
                return JS_ThrowTypeError(ctx, "serviceRegistry.describe requires 1 argument");
            }
            auto *registry = static_cast<HostServiceRegistry *>(JS_GetOpaque(this_val, js_service_registry_class_id));
            const char *name = JS_ToCString(ctx, argv[0]);
            PluginServiceDescriptor descriptor;
            const bool ok = registry && name && registry->describe_service(name, descriptor);
            if (name) JS_FreeCString(ctx, name);
            return ok ? make_service_descriptor(ctx, descriptor) : JS_NULL;
        }

        static JSValue js_service_registry_list(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            (void)argc;
            (void)argv;
            auto *registry = static_cast<HostServiceRegistry *>(JS_GetOpaque(this_val, js_service_registry_class_id));
            auto services = registry ? registry->list_services() : std::vector<PluginServiceDescriptor>{};
            JSValue arr = JS_NewArray(ctx);
            for (uint32_t i = 0; i < services.size(); ++i) {
                JS_SetPropertyUint32(ctx, arr, i, make_service_descriptor(ctx, services[i]));
            }
            return arr;
        }

        static JSValue js_service_registry_unregister_all(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            (void)argc;
            (void)argv;
            auto *registry = static_cast<HostServiceRegistry *>(JS_GetOpaque(this_val, js_service_registry_class_id));
            auto *ctx_info = get_ctx_info(ctx);
            if (registry && ctx_info) {
                registry->unregister_plugin_services(ctx_info->plugin_name);
            }
            return JS_UNDEFINED;
        }

        static const JSCFunctionListEntry js_service_registry_proto_funcs[] = {
            JS_CFUNC_DEF("register", 5, js_service_registry_register),
            JS_CFUNC_DEF("has", 1, js_service_registry_has),
            JS_CFUNC_DEF("describe", 1, js_service_registry_describe),
            JS_CFUNC_DEF("list", 0, js_service_registry_list),
            JS_CFUNC_DEF("unregisterAll", 0, js_service_registry_unregister_all),
        };

        static void js_service_registry_finalizer(JSRuntime *rt, JSValue val)
        {
        }

        // ---- HttpInterceptor binding ----

        static bool call_ts_http_callback(int cb_id, const std::string &path, const std::string &method)
        {
            TsEventCallback cb;
            {
                std::lock_guard<std::mutex> lock(g_ts_callbacks_mutex);
                auto it = g_ts_event_callbacks.find(cb_id);
                if (it == g_ts_event_callbacks.end()) {
                    return true;
                }
                cb = it->second;
            }
            if (!cb.js_mutex) {
                return true;
            }
            std::lock_guard<std::recursive_mutex> js_lock(*cb.js_mutex);
            JSContext *js_ctx = cb.ctx;
            if (cb.interrupt_data) {
                set_callback_deadline(cb.interrupt_data, cb.execution_timeout_ms);
            }
            JSValue req = JS_NewObject(js_ctx);
            JS_SetPropertyStr(js_ctx, req, "path", JS_NewString(js_ctx, path.c_str()));
            JS_SetPropertyStr(js_ctx, req, "method", JS_NewString(js_ctx, method.c_str()));
            JSValue argv[] = { req };
            JSValue ret = JS_Call(js_ctx, cb.callback, JS_UNDEFINED, 1, argv);
            if (cb.interrupt_data) {
                clear_callback_deadline(cb.interrupt_data);
            }
            bool keep_going = true;
            if (JS_IsException(ret)) {
                JSValue exception = JS_GetException(js_ctx);
                const char *err = JS_ToCString(js_ctx, exception);
                LOG_ERROR("ts http interceptor callback error: {}", err ? err : "unknown");
                JS_FreeCString(js_ctx, err);
                JS_FreeValue(js_ctx, exception);
            } else if (JS_IsBool(ret)) {
                keep_going = JS_ToBool(js_ctx, ret) != 0;
            }
            JS_FreeValue(js_ctx, ret);
            JS_FreeValue(js_ctx, req);
            return keep_going;
        }

        static int store_ts_callback(JSContext *ctx, JSValueConst callback, TsCtxInfo *ctx_info)
        {
            TsEventCallback cb;
            cb.ctx = ctx;
            cb.callback = JS_DupValue(ctx, callback);
            cb.plugin_name = ctx_info ? ctx_info->plugin_name : "";
            cb.execution_timeout_ms = ctx_info ? ctx_info->execution_timeout_ms : 0;
            cb.js_mutex = ctx_info ? ctx_info->js_mutex : nullptr;
            cb.interrupt_data = ctx_info ? ctx_info->interrupt_data : nullptr;
            std::lock_guard<std::mutex> lock(g_ts_callbacks_mutex);
            int cb_id = g_ts_next_callback_id++;
            g_ts_event_callbacks[cb_id] = std::move(cb);
            return cb_id;
        }

        static JSValue js_http_add_middleware(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
                return JS_ThrowTypeError(ctx, "httpInterceptor.addMiddleware requires callback");
            }
            auto *interceptor = static_cast<HostHttpInterceptor *>(JS_GetOpaque(this_val, js_http_interceptor_class_id));
            auto *ctx_info = get_ctx_info(ctx);
            if (!interceptor || !ctx_info || ctx_info->plugin_name.empty()) {
                return JS_NewInt32(ctx, 0);
            }
            const char *name = argc > 1 ? JS_ToCString(ctx, argv[1]) : nullptr;
            const int cb_id = store_ts_callback(ctx, argv[0], ctx_info);
            auto id = interceptor->add_middleware(
                ctx_info->plugin_name,
                [cb_id](const std::string &path, const std::string &method, void *, void *) {
                    return call_ts_http_callback(cb_id, path, method);
                },
                name ? name : "");
            if (id != 0 && ctx_info->resource_guard) {
                ctx_info->resource_guard->track(ctx_info->plugin_name,
                                               PluginResourceType::callback,
                                               [cb_id]() { (void)cleanup_event_callback_by_id(cb_id); },
                                               "callback:http-middleware");
            } else if (id == 0) {
                (void)cleanup_event_callback_by_id(cb_id);
            }
            if (name) JS_FreeCString(ctx, name);
            return JS_NewInt64(ctx, static_cast<int64_t>(id));
        }

        static JSValue js_http_add_route(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            if (argc < 2 || !JS_IsFunction(ctx, argv[1])) {
                return JS_ThrowTypeError(ctx, "httpInterceptor.addRoute requires path and callback");
            }
            auto *interceptor = static_cast<HostHttpInterceptor *>(JS_GetOpaque(this_val, js_http_interceptor_class_id));
            auto *ctx_info = get_ctx_info(ctx);
            if (!interceptor || !ctx_info || ctx_info->plugin_name.empty()) {
                return JS_NewInt32(ctx, 0);
            }
            const char *path = JS_ToCString(ctx, argv[0]);
            const char *method = argc > 2 ? JS_ToCString(ctx, argv[2]) : nullptr;
            const int cb_id = store_ts_callback(ctx, argv[1], ctx_info);
            auto id = interceptor->add_route(
                ctx_info->plugin_name,
                path ? path : "",
                [cb_id](const std::string &route_path, const std::string &route_method, void *, void *) {
                    (void)call_ts_http_callback(cb_id, route_path, route_method);
                },
                method ? method : "");
            if (id != 0 && ctx_info->resource_guard) {
                ctx_info->resource_guard->track(ctx_info->plugin_name,
                                               PluginResourceType::callback,
                                               [cb_id]() { (void)cleanup_event_callback_by_id(cb_id); },
                                               "callback:http-route");
            } else if (id == 0) {
                (void)cleanup_event_callback_by_id(cb_id);
            }
            if (path) JS_FreeCString(ctx, path);
            if (method) JS_FreeCString(ctx, method);
            return JS_NewInt64(ctx, static_cast<int64_t>(id));
        }

        static JSValue js_http_remove(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            if (argc < 1) {
                return JS_ThrowTypeError(ctx, "httpInterceptor.remove requires id");
            }
            auto *interceptor = static_cast<HostHttpInterceptor *>(JS_GetOpaque(this_val, js_http_interceptor_class_id));
            int64_t id = 0;
            JS_ToInt64(ctx, &id, argv[0]);
            return JS_NewBool(ctx, interceptor && interceptor->remove(static_cast<HttpInterceptorId>(id)));
        }

        static JSValue js_http_is_available(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            (void)ctx;
            (void)argc;
            (void)argv;
            auto *interceptor = static_cast<HostHttpInterceptor *>(JS_GetOpaque(this_val, js_http_interceptor_class_id));
            return JS_NewBool(ctx, interceptor && interceptor->is_available());
        }

        static const JSCFunctionListEntry js_http_interceptor_proto_funcs[] = {
            JS_CFUNC_DEF("addMiddleware", 2, js_http_add_middleware),
            JS_CFUNC_DEF("addRoute", 3, js_http_add_route),
            JS_CFUNC_DEF("remove", 1, js_http_remove),
            JS_CFUNC_DEF("isAvailable", 0, js_http_is_available),
        };

        static void js_http_interceptor_finalizer(JSRuntime *rt, JSValue val)
        {
        }

        static void js_ctx_info_finalizer(JSRuntime *rt, JSValue val)
        {
            auto *info = static_cast<TsCtxInfo *>(JS_GetOpaque(val, js_ctx_info_class_id));
            if (info) {
                delete info;
                JS_SetOpaque(val, nullptr);
            }
        }

        static void register_js_class(JSContext *ctx, JSClassID *class_id, const char *class_name,
                                      JSClassFinalizer *finalizer, const JSCFunctionListEntry *proto_funcs,
                                      int proto_funcs_len)
        {
            if (*class_id == 0) {
                JS_NewClassID(class_id);
            }
            JSClassDef class_def;
            memset(&class_def, 0, sizeof(class_def));
            class_def.class_name = class_name;
            class_def.finalizer = finalizer;
            JS_NewClass(JS_GetRuntime(ctx), *class_id, &class_def);

            JSValue proto = JS_NewObject(ctx);
            if (proto_funcs && proto_funcs_len > 0) {
                JS_SetPropertyFunctionList(ctx, proto, proto_funcs, proto_funcs_len);
            }
            JS_SetClassProto(ctx, *class_id, proto);
        }

        static JSValue make_instance(JSContext *ctx, JSClassID class_id, void *ptr)
        {
            JSValue proto = JS_GetClassProto(ctx, class_id);
            JSValue obj = JS_NewObjectProtoClass(ctx, proto, class_id);
            JS_FreeValue(ctx, proto);
            JS_SetOpaque(obj, ptr);
            return obj;
        }

    } // namespace

    JSValue push_json_to_js(JSContext * ctx, const nlohmann::json & j)
    {
        return make_json_value(ctx, j);
    }

    JSValue ts_register_host_modules(JSContext * ctx, const PluginContext & context,
                                     size_t execution_timeout_ms, std::recursive_mutex * js_mutex,
                                     TsInterruptData * interrupt_data)
    {
        auto *ctx_info = new TsCtxInfo();
        ctx_info->plugin_name = context.plugin_name;
        ctx_info->execution_timeout_ms = execution_timeout_ms;
        ctx_info->resource_guard = context.resource_guard;
        ctx_info->js_mutex = js_mutex;
        ctx_info->interrupt_data = interrupt_data;

        JSValue global = JS_GetGlobalObject(ctx);
        JSAtom ctx_key = JS_NewAtom(ctx, CTX_REGISTRY_KEY);
        register_js_class(ctx, &js_ctx_info_class_id, "TsCtxInfo", js_ctx_info_finalizer, nullptr, 0);
        JSValue ctx_info_proto = JS_GetClassProto(ctx, js_ctx_info_class_id);
        JSValue info_obj = JS_NewObjectProtoClass(ctx, ctx_info_proto, js_ctx_info_class_id);
        JS_FreeValue(ctx, ctx_info_proto);
        JS_SetOpaque(info_obj, ctx_info);
        JS_SetProperty(ctx, global, ctx_key, info_obj);
        JS_FreeAtom(ctx, ctx_key);
        JS_FreeValue(ctx, global);

        register_js_class(ctx, &js_logger_class_id, "HostLogger", js_logger_finalizer,
                          js_logger_proto_funcs, sizeof(js_logger_proto_funcs) / sizeof(js_logger_proto_funcs[0]));
        register_js_class(ctx, &js_storage_class_id, "HostStorage", js_storage_finalizer,
                          js_storage_proto_funcs, sizeof(js_storage_proto_funcs) / sizeof(js_storage_proto_funcs[0]));
        register_js_class(ctx, &js_eventbus_class_id, "HostEventBus", js_eventbus_finalizer,
                          js_eventbus_proto_funcs, sizeof(js_eventbus_proto_funcs) / sizeof(js_eventbus_proto_funcs[0]));
        register_js_class(ctx, &js_scheduler_class_id, "HostScheduler", js_scheduler_finalizer,
                          js_scheduler_proto_funcs, sizeof(js_scheduler_proto_funcs) / sizeof(js_scheduler_proto_funcs[0]));
        register_js_class(ctx, &js_service_registry_class_id, "HostServiceRegistry", js_service_registry_finalizer,
                          js_service_registry_proto_funcs, sizeof(js_service_registry_proto_funcs) / sizeof(js_service_registry_proto_funcs[0]));
        register_js_class(ctx, &js_http_interceptor_class_id, "HostHttpInterceptor", js_http_interceptor_finalizer,
                          js_http_interceptor_proto_funcs, sizeof(js_http_interceptor_proto_funcs) / sizeof(js_http_interceptor_proto_funcs[0]));

        JSValue host = JS_NewObject(ctx);

        if (context.logger) {
            JS_SetPropertyStr(ctx, host, "logger", make_instance(ctx, js_logger_class_id, context.logger));
        }

        if (context.storage) {
            JS_SetPropertyStr(ctx, host, "storage", make_instance(ctx, js_storage_class_id, context.storage));
        }

        if (context.event_bus) {
            JS_SetPropertyStr(ctx, host, "eventBus", make_instance(ctx, js_eventbus_class_id, context.event_bus));
        }

        if (context.scheduler) {
            JS_SetPropertyStr(ctx, host, "scheduler", make_instance(ctx, js_scheduler_class_id, context.scheduler));
        }

        if (context.service_registry) {
            JS_SetPropertyStr(ctx, host, "serviceRegistry", make_instance(ctx, js_service_registry_class_id, context.service_registry));
        }

        if (context.http_interceptor) {
            JS_SetPropertyStr(ctx, host, "httpInterceptor", make_instance(ctx, js_http_interceptor_class_id, context.http_interceptor));
        }

        JS_SetPropertyStr(ctx, host, "appName", JS_NewString(ctx, context.app_name.c_str()));
        JS_SetPropertyStr(ctx, host, "pluginName", JS_NewString(ctx, context.plugin_name.c_str()));

        if (context.config.loaded()) {
            auto *raw = context.config.raw();
            if (raw && raw->is_object()) {
                JS_SetPropertyStr(ctx, host, "config", make_json_object(ctx, *raw));
            }
        }

        return host;
    }

    void cleanup_ts_plugin_callbacks(JSContext * ctx, const std::string & plugin_name)
    {
        {
            std::lock_guard<std::mutex> lock(g_ts_callbacks_mutex);
            for (auto it = g_ts_event_callbacks.begin(); it != g_ts_event_callbacks.end();) {
                if (it->second.ctx == ctx && it->second.plugin_name == plugin_name) {
                    JS_FreeValue(ctx, it->second.callback);
                    it = g_ts_event_callbacks.erase(it);
                } else {
                    ++it;
                }
            }
            for (auto it = g_ts_scheduler_callbacks.begin(); it != g_ts_scheduler_callbacks.end();) {
                if (it->second.ctx == ctx && it->second.plugin_name == plugin_name) {
                    JS_FreeValue(ctx, it->second.callback);
                    it = g_ts_scheduler_callbacks.erase(it);
                } else {
                    ++it;
                }
            }
        }

        JSValue global = JS_GetGlobalObject(ctx);
        JSAtom ctx_key = JS_NewAtom(ctx, CTX_REGISTRY_KEY);
        JSValue val = JS_GetProperty(ctx, global, ctx_key);
        if (JS_IsObject(val)) {
            auto *info = static_cast<TsCtxInfo *>(JS_GetOpaque(val, js_ctx_info_class_id));
            if (info) {
                JS_SetOpaque(val, nullptr);
                delete info;
            }
        }
        JS_FreeValue(ctx, val);
        JS_DeleteProperty(ctx, global, ctx_key, 0);
        JS_FreeAtom(ctx, ctx_key);
        JS_FreeValue(ctx, global);
    }

} // namespace yuan::plugin
