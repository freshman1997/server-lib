#include "ts_script_plugin_adapter.h"
#include "ts_host_bindings.h"
#include "logger.h"

#include "quickjs_lib.h"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

namespace yuan::plugin
{

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
            cleanup_ts_plugin_callbacks(ctx_, manifest_.name);
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
            cleanup_ts_plugin_callbacks(ctx_, manifest_.name);
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

        JS_SetMemoryLimit(rt_, config_.memory_limit);
        JS_SetMaxStackSize(rt_, config_.max_stack_size);
        JS_SetInterruptHandler(rt_, js_interrupt_handler, &interrupt_data_);

        ctx_ = JS_NewContext(rt_);
        if (!ctx_) {
            LOG_ERROR("failed to create quickjs context for plugin '{}'", manifest_.name);
            JS_FreeRuntime(rt_);
            rt_ = nullptr;
            return false;
        }

        JS_AddIntrinsicBaseObjects(ctx_);
        JS_AddIntrinsicDate(ctx_);
        JS_AddIntrinsicRegExp(ctx_);
        JS_AddIntrinsicJSON(ctx_);
        JS_AddIntrinsicMapSet(ctx_);
        JS_AddIntrinsicTypedArrays(ctx_);
        JS_AddIntrinsicPromise(ctx_);

        apply_sandbox();
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
            cleanup_ts_plugin_callbacks(ctx_, manifest_.name);
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

} // namespace yuan::plugin
