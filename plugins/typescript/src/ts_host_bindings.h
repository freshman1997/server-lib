#ifndef __YUAN_PLUGIN_TS_HOST_BINDINGS_H__
#define __YUAN_PLUGIN_TS_HOST_BINDINGS_H__

#include "plugin/plugin_context.h"
#include "quickjs_lib.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>

namespace yuan::plugin
{

    struct TsInterruptData
    {
        std::chrono::steady_clock::time_point deadline;
        std::atomic<bool> timeout_triggered{ false };
    };

    JSValue ts_register_host_modules(JSContext * ctx, const PluginContext & context,
                                     size_t execution_timeout_ms, std::recursive_mutex * js_mutex,
                                     TsInterruptData * interrupt_data);

    JSValue push_json_to_js(JSContext * ctx, const nlohmann::json & j);

    void cleanup_ts_plugin_callbacks(JSContext * ctx, const std::string & plugin_name);

} // namespace yuan::plugin

#endif
