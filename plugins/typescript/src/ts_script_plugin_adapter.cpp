#include "ts_script_plugin_adapter.h"
#include "ts_host_bindings.h"
#include "logger.h"

#include "quickjs_lib.h"
#include <cassert>
#include "nlohmann/json.hpp"

#include <chrono>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace yuan::plugin
{

    namespace
    {
        class TsStreamProtocolHandler final : public PluginStreamProtocolHandler
        {
        public:
            TsStreamProtocolHandler(TsScriptPluginAdapter *adapter, std::string handler_name)
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
            TsScriptPluginAdapter *adapter_ = nullptr;
            std::string handler_name_;
        };

        class TsDatagramProtocolHandler final : public PluginDatagramProtocolHandler
        {
        public:
            TsDatagramProtocolHandler(TsScriptPluginAdapter *adapter, std::string handler_name)
                : adapter_(adapter),
                  handler_name_(std::move(handler_name))
            {
            }

            bool on_datagram(HostDatagramEndpoint &endpoint,
                             std::string_view peer,
                             std::span<const std::byte> bytes) override
            {
                return adapter_ &&
                       adapter_->call_datagram_protocol_handler(handler_name_, endpoint, peer, bytes);
            }

        private:
            TsScriptPluginAdapter *adapter_ = nullptr;
            std::string handler_name_;
        };

        struct PendingTsStreamPayload
        {
            std::string payload;
            bool consumed = false;
        };

        std::mutex g_pending_stream_payloads_mutex;
        std::unordered_map<const HostStreamConnection *, PendingTsStreamPayload> g_pending_stream_payloads;

        void set_pending_stream_payload(HostStreamConnection &connection, std::span<const std::byte> bytes)
        {
            PendingTsStreamPayload payload;
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

        JSClassID g_ts_stream_connection_class_id = 0;
        JSClassID g_ts_datagram_endpoint_class_id = 0;

        static JSValue js_stream_connection_id(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            (void)argc;
            (void)argv;
            auto *conn = static_cast<HostStreamConnection *>(JS_GetOpaque2(ctx, this_val, g_ts_stream_connection_class_id));
            if (!conn) {
                return JS_NewInt64(ctx, 0);
            }
            return JS_NewInt64(ctx, static_cast<int64_t>(conn->id()));
        }

        static JSValue js_stream_connection_peer_address(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            (void)argc;
            (void)argv;
            auto *conn = static_cast<HostStreamConnection *>(JS_GetOpaque2(ctx, this_val, g_ts_stream_connection_class_id));
            if (!conn) {
                return JS_NewString(ctx, "");
            }
            const auto peer = conn->peer_address();
            return JS_NewStringLen(ctx, peer.data(), peer.size());
        }

        static JSValue js_stream_connection_local_address(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            (void)argc;
            (void)argv;
            auto *conn = static_cast<HostStreamConnection *>(JS_GetOpaque2(ctx, this_val, g_ts_stream_connection_class_id));
            if (!conn) {
                return JS_NewString(ctx, "");
            }
            const auto local = conn->local_address();
            return JS_NewStringLen(ctx, local.data(), local.size());
        }

        static JSValue js_stream_connection_read(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            (void)argc;
            (void)argv;
            auto *conn = static_cast<HostStreamConnection *>(JS_GetOpaque2(ctx, this_val, g_ts_stream_connection_class_id));
            if (!conn) {
                return JS_NULL;
            }
            std::string payload;
            if (!consume_pending_stream_payload(*conn, payload)) {
                return JS_NULL;
            }
            return JS_NewStringLen(ctx, payload.data(), payload.size());
        }

        static JSValue js_stream_connection_read_line(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            (void)argc;
            (void)argv;
            auto *conn = static_cast<HostStreamConnection *>(JS_GetOpaque2(ctx, this_val, g_ts_stream_connection_class_id));
            if (!conn) {
                return JS_NULL;
            }
            std::string payload;
            if (!consume_pending_stream_payload(*conn, payload)) {
                return JS_NULL;
            }
            if (!payload.empty() && payload.back() == '\n') {
                payload.pop_back();
            }
            if (!payload.empty() && payload.back() == '\r') {
                payload.pop_back();
            }
            return JS_NewStringLen(ctx, payload.data(), payload.size());
        }

        static JSValue js_stream_connection_write(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            if (argc < 1) {
                return JS_ThrowTypeError(ctx, "connection.write requires 1 argument");
            }
            auto *conn = static_cast<HostStreamConnection *>(JS_GetOpaque2(ctx, this_val, g_ts_stream_connection_class_id));
            if (!conn) {
                return JS_NewBool(ctx, false);
            }
            size_t len = 0;
            const char *text = JS_ToCStringLen(ctx, &len, argv[0]);
            if (!text) {
                return JS_NewBool(ctx, false);
            }
            const auto *ptr = reinterpret_cast<const std::byte *>(text);
            const bool ok = conn->write(std::span<const std::byte>(ptr, len));
            JS_FreeCString(ctx, text);
            return JS_NewBool(ctx, ok);
        }

        static JSValue js_stream_connection_flush(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            (void)argc;
            (void)argv;
            auto *conn = static_cast<HostStreamConnection *>(JS_GetOpaque2(ctx, this_val, g_ts_stream_connection_class_id));
            return JS_NewBool(ctx, conn && conn->flush());
        }

        static JSValue js_stream_connection_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            (void)argc;
            (void)argv;
            auto *conn = static_cast<HostStreamConnection *>(JS_GetOpaque2(ctx, this_val, g_ts_stream_connection_class_id));
            if (conn) {
                conn->close();
            }
            return JS_UNDEFINED;
        }

        static JSValue js_stream_connection_is_open(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
        {
            (void)argc;
            (void)argv;
            auto *conn = static_cast<HostStreamConnection *>(JS_GetOpaque2(ctx, this_val, g_ts_stream_connection_class_id));
            return JS_NewBool(ctx, conn && conn->is_open());
        }

        static void js_stream_connection_finalizer(JSRuntime *rt, JSValue val)
        {
            (void)rt;
            (void)val;
        }

        void ensure_stream_connection_class(JSContext *ctx)
        {
            if (g_ts_stream_connection_class_id == 0) {
                JS_NewClassID(&g_ts_stream_connection_class_id);
            }

            JSValue existing_proto = JS_GetClassProto(ctx, g_ts_stream_connection_class_id);
            if (!JS_IsUndefined(existing_proto) && !JS_IsNull(existing_proto)) {
                JS_FreeValue(ctx, existing_proto);
                return;
            }
            JS_FreeValue(ctx, existing_proto);

            JSClassDef class_def{};
            class_def.class_name = "TsHostStreamConnection";
            class_def.finalizer = js_stream_connection_finalizer;
            (void)JS_NewClass(JS_GetRuntime(ctx), g_ts_stream_connection_class_id, &class_def);

            JSValue proto = JS_NewObject(ctx);
            static const JSCFunctionListEntry funcs[] = {
                JS_CFUNC_DEF("id", 0, js_stream_connection_id),
                JS_CFUNC_DEF("peerAddress", 0, js_stream_connection_peer_address),
                JS_CFUNC_DEF("localAddress", 0, js_stream_connection_local_address),
                JS_CFUNC_DEF("read", 1, js_stream_connection_read),
                JS_CFUNC_DEF("readLine", 1, js_stream_connection_read_line),
                JS_CFUNC_DEF("write", 1, js_stream_connection_write),
                JS_CFUNC_DEF("flush", 0, js_stream_connection_flush),
                JS_CFUNC_DEF("close", 0, js_stream_connection_close),
                JS_CFUNC_DEF("isOpen", 0, js_stream_connection_is_open),
            };
            JS_SetPropertyFunctionList(ctx, proto, funcs, sizeof(funcs) / sizeof(funcs[0]));
            JS_SetClassProto(ctx, g_ts_stream_connection_class_id, proto);
        }

        JSValue make_stream_connection_instance(JSContext *ctx, HostStreamConnection &connection)
        {
            ensure_stream_connection_class(ctx);
            JSValue proto = JS_GetClassProto(ctx, g_ts_stream_connection_class_id);
            JSValue obj = JS_NewObjectProtoClass(ctx, proto, g_ts_stream_connection_class_id);
            JS_FreeValue(ctx, proto);
            JS_SetOpaque(obj, &connection);
            return obj;
        }

        static JSValue js_datagram_endpoint_local_address(JSContext *ctx,
                                                          JSValueConst this_val,
                                                          int argc,
                                                          JSValueConst *argv)
        {
            (void)argc;
            (void)argv;
            auto *endpoint =
                static_cast<HostDatagramEndpoint *>(JS_GetOpaque2(ctx, this_val, g_ts_datagram_endpoint_class_id));
            if (!endpoint) {
                return JS_NewString(ctx, "");
            }
            const auto local = endpoint->local_address();
            return JS_NewStringLen(ctx, local.data(), local.size());
        }

        static JSValue js_datagram_endpoint_send_to(JSContext *ctx,
                                                    JSValueConst this_val,
                                                    int argc,
                                                    JSValueConst *argv)
        {
            if (argc < 2) {
                return JS_ThrowTypeError(ctx, "endpoint.sendTo requires 2 arguments");
            }
            auto *endpoint =
                static_cast<HostDatagramEndpoint *>(JS_GetOpaque2(ctx, this_val, g_ts_datagram_endpoint_class_id));
            if (!endpoint) {
                return JS_NewBool(ctx, false);
            }

            size_t peer_len = 0;
            size_t payload_len = 0;
            const char *peer = JS_ToCStringLen(ctx, &peer_len, argv[0]);
            if (!peer) {
                return JS_NewBool(ctx, false);
            }
            const char *payload = JS_ToCStringLen(ctx, &payload_len, argv[1]);
            if (!payload) {
                JS_FreeCString(ctx, peer);
                return JS_NewBool(ctx, false);
            }

            const auto *payload_ptr = reinterpret_cast<const std::byte *>(payload);
            const bool sent = endpoint->send_to(
                std::string_view(peer, peer_len),
                std::span<const std::byte>(payload_ptr, payload_len));
            JS_FreeCString(ctx, payload);
            JS_FreeCString(ctx, peer);
            return JS_NewBool(ctx, sent);
        }

        static void js_datagram_endpoint_finalizer(JSRuntime *rt, JSValue val)
        {
            (void)rt;
            (void)val;
        }

        void ensure_datagram_endpoint_class(JSContext *ctx)
        {
            if (g_ts_datagram_endpoint_class_id == 0) {
                JS_NewClassID(&g_ts_datagram_endpoint_class_id);
            }

            JSValue existing_proto = JS_GetClassProto(ctx, g_ts_datagram_endpoint_class_id);
            if (!JS_IsUndefined(existing_proto) && !JS_IsNull(existing_proto)) {
                JS_FreeValue(ctx, existing_proto);
                return;
            }
            JS_FreeValue(ctx, existing_proto);

            JSClassDef class_def{};
            class_def.class_name = "TsHostDatagramEndpoint";
            class_def.finalizer = js_datagram_endpoint_finalizer;
            (void)JS_NewClass(JS_GetRuntime(ctx), g_ts_datagram_endpoint_class_id, &class_def);

            JSValue proto = JS_NewObject(ctx);
            static const JSCFunctionListEntry funcs[] = {
                JS_CFUNC_DEF("localAddress", 0, js_datagram_endpoint_local_address),
                JS_CFUNC_DEF("sendTo", 2, js_datagram_endpoint_send_to),
            };
            JS_SetPropertyFunctionList(ctx, proto, funcs, sizeof(funcs) / sizeof(funcs[0]));
            JS_SetClassProto(ctx, g_ts_datagram_endpoint_class_id, proto);
        }

        JSValue make_datagram_endpoint_instance(JSContext *ctx, HostDatagramEndpoint &endpoint)
        {
            ensure_datagram_endpoint_class(ctx);
            JSValue proto = JS_GetClassProto(ctx, g_ts_datagram_endpoint_class_id);
            JSValue obj = JS_NewObjectProtoClass(ctx, proto, g_ts_datagram_endpoint_class_id);
            JS_FreeValue(ctx, proto);
            JS_SetOpaque(obj, &endpoint);
            return obj;
        }

        std::string fallback_handler_name(const std::string &handler_name)
        {
            const auto pos = handler_name.find_last_of('.');
            if (pos == std::string::npos || pos + 1 >= handler_name.size()) {
                return {};
            }
            return handler_name.substr(pos + 1);
        }

        bool resolve_handler_function(JSContext *ctx,
                                      const std::string &handler_name,
                                      JSValue *out_function,
                                      bool *function_missing)
        {
            if (out_function) {
                *out_function = JS_UNDEFINED;
            }
            if (function_missing) {
                *function_missing = false;
            }

            JSValue global = JS_GetGlobalObject(ctx);
            JSValue fn = JS_GetPropertyStr(ctx, global, handler_name.c_str());
            if (JS_IsFunction(ctx, fn)) {
                if (out_function) {
                    *out_function = fn;
                } else {
                    JS_FreeValue(ctx, fn);
                }
                JS_FreeValue(ctx, global);
                return true;
            }
            JS_FreeValue(ctx, fn);

            const auto fallback = fallback_handler_name(handler_name);
            if (!fallback.empty()) {
                JSValue fallback_fn = JS_GetPropertyStr(ctx, global, fallback.c_str());
                if (JS_IsFunction(ctx, fallback_fn)) {
                    if (out_function) {
                        *out_function = fallback_fn;
                    } else {
                        JS_FreeValue(ctx, fallback_fn);
                    }
                    JS_FreeValue(ctx, global);
                    return true;
                }
                JS_FreeValue(ctx, fallback_fn);
            }

            JS_FreeValue(ctx, global);
            if (function_missing) {
                *function_missing = true;
            }
            return false;
        }
    } // namespace

    TsScriptPluginAdapter::TsScriptPluginAdapter(const PluginManifest & manifest)
        : ScriptPluginAdapter(manifest)
    {
        memory_budget_.limit = config_.memory_limit;
    }

    TsScriptPluginAdapter::TsScriptPluginAdapter(const PluginManifest & manifest, const Config & config)
        : ScriptPluginAdapter(manifest), config_(config)
    {
        memory_budget_.limit = config_.memory_limit;
    }

    TsScriptPluginAdapter::~TsScriptPluginAdapter()
    {
        std::lock_guard<std::recursive_mutex> lock(js_mutex_);
        if (ctx_) {
            cleanup_ts_plugin_callbacks(ctx_, callback_owner_name());
        }
        if (ctx_) {
            JS_FreeContext(ctx_);
            ctx_ = nullptr;
        }
        if (rt_) {
            JS_FreeRuntime(rt_);
            rt_ = nullptr;
        }
    }

    int TsScriptPluginAdapter::js_interrupt_handler(JSRuntime * rt, void * opaque)
    {
        auto *data = static_cast<TsInterruptData *>(opaque);
        if (data->timeout_triggered.load(std::memory_order_relaxed)) {
            return 1;
        }
        if (std::chrono::steady_clock::now() >= data->deadline) {
            data->timeout_triggered.store(true, std::memory_order_relaxed);
            return 1;
        }
        return 0;
    }

    void TsScriptPluginAdapter::set_execution_deadline() const
    {
        interrupt_data_.timeout_triggered.store(false, std::memory_order_relaxed);
        if (config_.execution_timeout_ms > 0) {
            interrupt_data_.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.execution_timeout_ms);
        } else {
            interrupt_data_.deadline = std::chrono::steady_clock::time_point::max();
        }
    }

    void TsScriptPluginAdapter::clear_execution_deadline() const
    {
        interrupt_data_.deadline = std::chrono::steady_clock::time_point::max();
    }

    bool TsScriptPluginAdapter::init_js_engine()
    {
        if (rt_) {
            cleanup_ts_plugin_callbacks(ctx_, callback_owner_name());
            JS_FreeContext(ctx_);
            ctx_ = nullptr;
            JS_FreeRuntime(rt_);
            rt_ = nullptr;
        }

        rt_ = JS_NewRuntime();
        if (!rt_) {
            LOG_ERROR("failed to create quickjs runtime for plugin '{}'", manifest_.name);
            return false;
        }

        JS_SetMaxStackSize(rt_, config_.max_stack_size);
        JS_SetInterruptHandler(rt_, js_interrupt_handler, &interrupt_data_);

        ctx_ = JS_NewContextRaw(rt_);
        if (!ctx_) {
            LOG_ERROR("failed to create quickjs context for plugin '{}'", manifest_.name);
            JS_FreeRuntime(rt_);
            rt_ = nullptr;
            return false;
        }

        JS_AddIntrinsicBaseObjects(ctx_);
        JS_AddIntrinsicEval(ctx_);
        JS_AddIntrinsicDate(ctx_);
        JS_AddIntrinsicRegExp(ctx_);
        JS_AddIntrinsicJSON(ctx_);
        JS_AddIntrinsicMapSet(ctx_);
        JS_AddIntrinsicTypedArrays(ctx_);
        JS_AddIntrinsicPromise(ctx_);

        apply_sandbox();

        JSMemoryUsage usage;
        JS_ComputeMemoryUsage(rt_, &usage);
        const auto bootstrap_used = static_cast<size_t>(usage.malloc_size);
        const auto extra_budget = config_.memory_limit;
        const auto runtime_limit = extra_budget > std::numeric_limits<size_t>::max() - bootstrap_used
                                       ? std::numeric_limits<size_t>::max()
                                       : bootstrap_used + extra_budget;
        JS_SetMemoryLimit(rt_, runtime_limit);
        memory_budget_.limit = runtime_limit;

        return true;
    }

    void TsScriptPluginAdapter::apply_sandbox()
    {
        JSValue global = JS_GetGlobalObject(ctx_);

        const char *removed[] = { "eval", "Function" };
        for (const char *name : removed) {
            JSAtom atom = JS_NewAtom(ctx_, name);
            JS_DeleteProperty(ctx_, global, atom, 0);
            JS_FreeAtom(ctx_, atom);
        }

        JS_FreeValue(ctx_, global);
    }

    bool TsScriptPluginAdapter::load_script(const std::string & script_path)
    {
        if (!init_js_engine()) {
            return false;
        }

        std::ifstream file(script_path);
        if (!file.good()) {
            LOG_ERROR("failed to open js/ts script '{}'", script_path);
            JS_FreeContext(ctx_);
            ctx_ = nullptr;
            JS_FreeRuntime(rt_);
            rt_ = nullptr;
            return false;
        }

        std::ostringstream ss;
        ss << file.rdbuf();
        std::string script_content = ss.str();

        set_execution_deadline();

        int eval_flags = JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT;
        JSValue result = JS_Eval(ctx_, script_content.c_str(), script_content.size(),
                                 script_path.c_str(), eval_flags);

        if (interrupt_data_.timeout_triggered.load(std::memory_order_relaxed)) {
            LOG_ERROR("script '{}' execution timed out", script_path);
            if (JS_IsException(result)) {
                JSValue exception = JS_GetException(ctx_);
                JS_FreeValue(ctx_, exception);
            }
            JS_FreeValue(ctx_, result);
            JS_FreeContext(ctx_);
            ctx_ = nullptr;
            JS_FreeRuntime(rt_);
            rt_ = nullptr;
            return false;
        }

        if (JS_IsException(result)) {
            JSValue exception = JS_GetException(ctx_);
            const char *err = JS_ToCString(ctx_, exception);
            LOG_ERROR("failed to evaluate script '{}': {}", script_path, err ? err : "unknown");
            JS_FreeCString(ctx_, err);
            JS_FreeValue(ctx_, exception);
            JS_FreeValue(ctx_, result);
            JS_FreeContext(ctx_);
            ctx_ = nullptr;
            JS_FreeRuntime(rt_);
            rt_ = nullptr;
            return false;
        }

        JSValue global = JS_GetGlobalObject(ctx_);
        JSAtom on_init_atom = JS_NewAtom(ctx_, "on_init");
        JSValue on_init_fn = JS_GetProperty(ctx_, global, on_init_atom);

        if (!JS_IsFunction(ctx_, on_init_fn)) {
            LOG_ERROR("script '{}' must define on_init function", script_path);
            JS_FreeValue(ctx_, on_init_fn);
            JS_FreeAtom(ctx_, on_init_atom);
            JS_FreeValue(ctx_, global);
            JS_FreeValue(ctx_, result);
            JS_FreeContext(ctx_);
            ctx_ = nullptr;
            JS_FreeRuntime(rt_);
            rt_ = nullptr;
            return false;
        }

        JS_FreeValue(ctx_, on_init_fn);
        JS_FreeAtom(ctx_, on_init_atom);
        JS_FreeValue(ctx_, global);
        JS_FreeValue(ctx_, result);

        clear_execution_deadline();
        script_loaded_ = true;
        return true;
    }

    void TsScriptPluginAdapter::register_protocol_handlers(PluginProtocolHandlerRegistry &registry)
    {
        std::unordered_set<std::string> seen_stream_handlers;
        std::unordered_set<std::string> seen_datagram_handlers;
        for (const auto &service : manifest_.protocol_services) {
            if (service.handler.empty() || service.language != "typescript") {
                continue;
            }

            const std::string handler_name = service.handler;
            const std::string transport =
                !service.transport.empty() ? service.transport : service.protocol;
            if (transport == "udp") {
                if (!seen_datagram_handlers.insert(handler_name).second) {
                    continue;
                }
                if (!has_datagram_protocol_handler(handler_name)) {
                    LOG_WARN("ts plugin '{}' skipped datagram protocol handler '{}' because function is missing",
                             manifest_.name,
                             handler_name);
                    continue;
                }
                (void)registry.register_datagram_handler(
                    handler_name,
                    [this, handler_name](const ProtocolServiceDescriptor &) {
                        return std::make_unique<TsDatagramProtocolHandler>(this, handler_name);
                    });
                continue;
            }

            if (!seen_stream_handlers.insert(handler_name).second) {
                continue;
            }
            if (!has_stream_protocol_handler(handler_name)) {
                LOG_WARN("ts plugin '{}' skipped protocol handler '{}' because function is missing",
                         manifest_.name,
                         handler_name);
                continue;
            }
            (void)registry.register_stream_handler(
                handler_name,
                [this, handler_name](const ProtocolServiceDescriptor &) {
                    return std::make_unique<TsStreamProtocolHandler>(this, handler_name);
                });
        }
    }

    bool TsScriptPluginAdapter::has_stream_protocol_handler(const std::string &handler_name) const
    {
        std::lock_guard<std::recursive_mutex> lock(js_mutex_);
        if (!ctx_ || !rt_) {
            return false;
        }

        bool function_missing = false;
        JSValue fn = JS_UNDEFINED;
        const bool found = resolve_handler_function(ctx_, handler_name, &fn, &function_missing);
        if (found) {
            JS_FreeValue(ctx_, fn);
        }
        return found && !function_missing;
    }

    bool TsScriptPluginAdapter::call_stream_protocol_handler(const std::string &handler_name,
                                                             HostStreamConnection &connection,
                                                             std::span<const std::byte> bytes)
    {
        return call_js_stream_handler(handler_name, connection, bytes, nullptr);
    }

    bool TsScriptPluginAdapter::has_datagram_protocol_handler(const std::string &handler_name) const
    {
        std::lock_guard<std::recursive_mutex> lock(js_mutex_);
        if (!ctx_ || !rt_) {
            return false;
        }

        bool function_missing = false;
        JSValue fn = JS_UNDEFINED;
        const bool found = resolve_handler_function(ctx_, handler_name, &fn, &function_missing);
        if (found) {
            JS_FreeValue(ctx_, fn);
        }
        return found && !function_missing;
    }

    bool TsScriptPluginAdapter::call_datagram_protocol_handler(const std::string &handler_name,
                                                               HostDatagramEndpoint &endpoint,
                                                               std::string_view peer,
                                                               std::span<const std::byte> bytes)
    {
        return call_js_datagram_handler(handler_name, endpoint, peer, bytes, nullptr);
    }

    void TsScriptPluginAdapter::call_js_void(const char * func_name)
    {
        std::lock_guard<std::recursive_mutex> lock(js_mutex_);
        if (!ctx_ || !rt_)
            return;

        JSValue global = JS_GetGlobalObject(ctx_);
        JSAtom atom = JS_NewAtom(ctx_, func_name);
        JSValue fn = JS_GetProperty(ctx_, global, atom);

        if (!JS_IsFunction(ctx_, fn)) {
            JS_FreeValue(ctx_, fn);
            JS_FreeAtom(ctx_, atom);
            JS_FreeValue(ctx_, global);
            return;
        }

        set_execution_deadline();
        JSValue ret = JS_Call(ctx_, fn, JS_UNDEFINED, 0, nullptr);
        clear_execution_deadline();

        if (interrupt_data_.timeout_triggered.load(std::memory_order_relaxed)) {
            LOG_ERROR("ts plugin '{}' {} timed out", manifest_.name, func_name);
            if (JS_IsException(ret)) {
                JSValue exception = JS_GetException(ctx_);
                JS_FreeValue(ctx_, exception);
            }
        } else if (JS_IsException(ret)) {
            JSValue exception = JS_GetException(ctx_);
            const char *err = JS_ToCString(ctx_, exception);
            LOG_ERROR("ts plugin '{}' {}: {}", manifest_.name, func_name, err ? err : "unknown");
            JS_FreeCString(ctx_, err);
            JS_FreeValue(ctx_, exception);
        }

        JS_FreeValue(ctx_, ret);
        JS_FreeValue(ctx_, fn);
        JS_FreeAtom(ctx_, atom);
        JS_FreeValue(ctx_, global);
    }

    bool TsScriptPluginAdapter::call_js_init(const PluginContext & context)
    {
        std::lock_guard<std::recursive_mutex> lock(js_mutex_);
        if (!ctx_ || !rt_)
            return false;

        JSValue global = JS_GetGlobalObject(ctx_);
        JSAtom atom = JS_NewAtom(ctx_, "on_init");
        JSValue fn = JS_GetProperty(ctx_, global, atom);

        if (!JS_IsFunction(ctx_, fn)) {
            JS_FreeValue(ctx_, fn);
            JS_FreeAtom(ctx_, atom);
            JS_FreeValue(ctx_, global);
            return false;
        }

        JSValue host_obj = ts_register_host_modules(ctx_, context, config_.execution_timeout_ms, &js_mutex_, &interrupt_data_);

        set_execution_deadline();
        JSValue ret = JS_Call(ctx_, fn, JS_UNDEFINED, 1, &host_obj);
        clear_execution_deadline();

        bool result = false;
        if (interrupt_data_.timeout_triggered.load(std::memory_order_relaxed)) {
            LOG_ERROR("ts plugin '{}' on_init timed out", manifest_.name);
            if (JS_IsException(ret)) {
                JSValue exception = JS_GetException(ctx_);
                JS_FreeValue(ctx_, exception);
            }
        } else if (JS_IsException(ret)) {
            JSValue exception = JS_GetException(ctx_);
            const char *err = JS_ToCString(ctx_, exception);
            LOG_ERROR("ts plugin '{}' on_init error: {}", manifest_.name, err ? err : "unknown");
            JS_FreeCString(ctx_, err);
            JS_FreeValue(ctx_, exception);
        } else {
            result = JS_ToBool(ctx_, ret) > 0;
        }

        JS_FreeValue(ctx_, ret);
        JS_FreeValue(ctx_, host_obj);
        JS_FreeValue(ctx_, fn);
        JS_FreeAtom(ctx_, atom);
        JS_FreeValue(ctx_, global);
        return result;
    }

    void TsScriptPluginAdapter::call_js_config_changed(const PluginConfigView & config)
    {
        std::lock_guard<std::recursive_mutex> lock(js_mutex_);
        if (!ctx_ || !rt_)
            return;

        JSValue global = JS_GetGlobalObject(ctx_);
        JSAtom atom = JS_NewAtom(ctx_, "on_config_changed");
        JSValue fn = JS_GetProperty(ctx_, global, atom);

        if (!JS_IsFunction(ctx_, fn)) {
            JS_FreeValue(ctx_, fn);
            JS_FreeAtom(ctx_, atom);
            JS_FreeValue(ctx_, global);
            return;
        }

        JSValue config_val;
        auto *raw = config.raw();
        if (raw && raw->is_object()) {
            config_val = push_json_to_js(ctx_, *raw);
        } else {
            config_val = JS_NULL;
        }

        set_execution_deadline();
        JSValue ret = JS_Call(ctx_, fn, JS_UNDEFINED, 1, &config_val);
        clear_execution_deadline();

        if (interrupt_data_.timeout_triggered.load(std::memory_order_relaxed)) {
            LOG_ERROR("ts plugin '{}' on_config_changed timed out", manifest_.name);
            if (JS_IsException(ret)) {
                JSValue exception = JS_GetException(ctx_);
                JS_FreeValue(ctx_, exception);
            }
        } else if (JS_IsException(ret)) {
            JSValue exception = JS_GetException(ctx_);
            const char *err = JS_ToCString(ctx_, exception);
            LOG_ERROR("ts plugin '{}' on_config_changed error: {}", manifest_.name, err ? err : "unknown");
            JS_FreeCString(ctx_, err);
            JS_FreeValue(ctx_, exception);
        }

        JS_FreeValue(ctx_, ret);
        JS_FreeValue(ctx_, config_val);
        JS_FreeValue(ctx_, fn);
        JS_FreeAtom(ctx_, atom);
        JS_FreeValue(ctx_, global);
    }

    bool TsScriptPluginAdapter::call_js_stream_handler(const std::string &handler_name,
                                                       HostStreamConnection &connection,
                                                       std::span<const std::byte> bytes,
                                                       bool *function_missing) const
    {
        std::lock_guard<std::recursive_mutex> lock(js_mutex_);
        if (!ctx_ || !rt_) {
            if (function_missing) {
                *function_missing = true;
            }
            return false;
        }

        JSValue fn = JS_UNDEFINED;
        if (!resolve_handler_function(ctx_, handler_name, &fn, function_missing)) {
            return false;
        }

        set_pending_stream_payload(connection, bytes);
        JSValue conn_obj = make_stream_connection_instance(ctx_, connection);
        const char *payload = reinterpret_cast<const char *>(bytes.data());
        JSValue payload_val = JS_NewStringLen(ctx_, payload ? payload : "", bytes.size());
        JSValue argv[2] = { conn_obj, payload_val };

        set_execution_deadline();
        JSValue ret = JS_Call(ctx_, fn, JS_UNDEFINED, 2, argv);
        clear_execution_deadline();

        if (interrupt_data_.timeout_triggered.load(std::memory_order_relaxed)) {
            LOG_ERROR("ts plugin '{}' protocol handler '{}' timed out", manifest_.name, handler_name);
            if (JS_IsException(ret)) {
                JSValue exception = JS_GetException(ctx_);
                JS_FreeValue(ctx_, exception);
            }
            JS_FreeValue(ctx_, ret);
            JS_FreeValue(ctx_, payload_val);
            JS_FreeValue(ctx_, conn_obj);
            JS_FreeValue(ctx_, fn);
            clear_pending_stream_payload(connection);
            return false;
        }

        if (JS_IsException(ret)) {
            JSValue exception = JS_GetException(ctx_);
            const char *err = JS_ToCString(ctx_, exception);
            LOG_ERROR("ts plugin '{}' protocol handler '{}' error: {}",
                      manifest_.name,
                      handler_name,
                      err ? err : "unknown");
            JS_FreeCString(ctx_, err);
            JS_FreeValue(ctx_, exception);
            JS_FreeValue(ctx_, ret);
            JS_FreeValue(ctx_, payload_val);
            JS_FreeValue(ctx_, conn_obj);
            JS_FreeValue(ctx_, fn);
            clear_pending_stream_payload(connection);
            return false;
        }

        bool keep_open = true;
        if (JS_IsBool(ret)) {
            keep_open = JS_ToBool(ctx_, ret) > 0;
        } else if (JS_IsNull(ret) || JS_IsUndefined(ret)) {
            keep_open = true;
        } else {
            keep_open = JS_ToBool(ctx_, ret) > 0;
        }

        JS_FreeValue(ctx_, ret);
        JS_FreeValue(ctx_, payload_val);
        JS_FreeValue(ctx_, conn_obj);
        JS_FreeValue(ctx_, fn);
        clear_pending_stream_payload(connection);
        return keep_open;
    }

    bool TsScriptPluginAdapter::call_js_datagram_handler(const std::string &handler_name,
                                                         HostDatagramEndpoint &endpoint,
                                                         std::string_view peer,
                                                         std::span<const std::byte> bytes,
                                                         bool *function_missing) const
    {
        std::lock_guard<std::recursive_mutex> lock(js_mutex_);
        if (!ctx_ || !rt_) {
            if (function_missing) {
                *function_missing = true;
            }
            return false;
        }

        JSValue fn = JS_UNDEFINED;
        if (!resolve_handler_function(ctx_, handler_name, &fn, function_missing)) {
            return false;
        }

        JSValue endpoint_obj = make_datagram_endpoint_instance(ctx_, endpoint);
        JSValue peer_val = JS_NewStringLen(ctx_, peer.data(), peer.size());
        const char *payload = reinterpret_cast<const char *>(bytes.data());
        JSValue payload_val = JS_NewStringLen(ctx_, payload ? payload : "", bytes.size());
        JSValue argv[3] = { endpoint_obj, peer_val, payload_val };

        set_execution_deadline();
        JSValue ret = JS_Call(ctx_, fn, JS_UNDEFINED, 3, argv);
        clear_execution_deadline();

        if (interrupt_data_.timeout_triggered.load(std::memory_order_relaxed)) {
            LOG_ERROR("ts plugin '{}' protocol datagram handler '{}' timed out", manifest_.name, handler_name);
            if (JS_IsException(ret)) {
                JSValue exception = JS_GetException(ctx_);
                JS_FreeValue(ctx_, exception);
            }
            JS_FreeValue(ctx_, ret);
            JS_FreeValue(ctx_, payload_val);
            JS_FreeValue(ctx_, peer_val);
            JS_FreeValue(ctx_, endpoint_obj);
            JS_FreeValue(ctx_, fn);
            return false;
        }

        if (JS_IsException(ret)) {
            JSValue exception = JS_GetException(ctx_);
            const char *err = JS_ToCString(ctx_, exception);
            LOG_ERROR("ts plugin '{}' protocol datagram handler '{}' error: {}",
                      manifest_.name,
                      handler_name,
                      err ? err : "unknown");
            JS_FreeCString(ctx_, err);
            JS_FreeValue(ctx_, exception);
            JS_FreeValue(ctx_, ret);
            JS_FreeValue(ctx_, payload_val);
            JS_FreeValue(ctx_, peer_val);
            JS_FreeValue(ctx_, endpoint_obj);
            JS_FreeValue(ctx_, fn);
            return false;
        }

        bool keep_open = true;
        if (JS_IsBool(ret)) {
            keep_open = JS_ToBool(ctx_, ret) > 0;
        } else if (JS_IsNull(ret) || JS_IsUndefined(ret)) {
            keep_open = true;
        } else {
            keep_open = JS_ToBool(ctx_, ret) > 0;
        }

        JS_FreeValue(ctx_, ret);
        JS_FreeValue(ctx_, payload_val);
        JS_FreeValue(ctx_, peer_val);
        JS_FreeValue(ctx_, endpoint_obj);
        JS_FreeValue(ctx_, fn);
        return keep_open;
    }

    bool TsScriptPluginAdapter::do_init(const PluginContext & context)
    {
        return call_js_init(context);
    }

    void TsScriptPluginAdapter::do_enable()
    {
        call_js_void("on_enable");
    }

    void TsScriptPluginAdapter::do_disable()
    {
        call_js_void("on_disable");
    }

    void TsScriptPluginAdapter::do_release()
    {
        std::lock_guard<std::recursive_mutex> lock(js_mutex_);
        call_js_void("on_release");
        if (ctx_) {
            cleanup_ts_plugin_callbacks(ctx_, callback_owner_name());
        }
        if (ctx_) {
            JS_FreeContext(ctx_);
            ctx_ = nullptr;
        }
        if (rt_) {
            JS_FreeRuntime(rt_);
            rt_ = nullptr;
        }
    }

    bool TsScriptPluginAdapter::do_health_check() const
    {
        std::lock_guard<std::recursive_mutex> lock(js_mutex_);
        if (!ctx_ || !rt_) {
            return true;
        }

        JSValue global = JS_GetGlobalObject(ctx_);
        JSAtom atom = JS_NewAtom(ctx_, "on_health_check");
        JSValue fn = JS_GetProperty(ctx_, global, atom);

        if (!JS_IsFunction(ctx_, fn)) {
            JS_FreeValue(ctx_, fn);
            JS_FreeAtom(ctx_, atom);
            JS_FreeValue(ctx_, global);
            return true;
        }

        set_execution_deadline();
        JSValue ret = JS_Call(ctx_, fn, JS_UNDEFINED, 0, nullptr);
        clear_execution_deadline();

        bool result = true;
        if (interrupt_data_.timeout_triggered.load(std::memory_order_relaxed)) {
            LOG_ERROR("ts plugin '{}' on_health_check timed out", manifest_.name);
            if (JS_IsException(ret)) {
                JSValue exception = JS_GetException(ctx_);
                JS_FreeValue(ctx_, exception);
            }
            result = false;
        } else if (JS_IsException(ret)) {
            JSValue exception = JS_GetException(ctx_);
            const char *err = JS_ToCString(ctx_, exception);
            LOG_ERROR("ts plugin '{}' on_health_check error: {}", manifest_.name, err ? err : "unknown");
            JS_FreeCString(ctx_, err);
            JS_FreeValue(ctx_, exception);
            result = false;
        } else {
            result = JS_ToBool(ctx_, ret) > 0;
        }

        JS_FreeValue(ctx_, ret);
        JS_FreeValue(ctx_, fn);
        JS_FreeAtom(ctx_, atom);
        JS_FreeValue(ctx_, global);
        return result;
    }

    void TsScriptPluginAdapter::do_config_changed(const PluginConfigView & config)
    {
        call_js_config_changed(config);
    }

    std::string TsScriptPluginAdapter::callback_owner_name() const
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
