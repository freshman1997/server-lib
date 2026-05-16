#ifndef __YUAN_PLUGIN_TS_SCRIPT_PLUGIN_ADAPTER_H__
#define __YUAN_PLUGIN_TS_SCRIPT_PLUGIN_ADAPTER_H__

#include "plugin/script_plugin_adapter.h"
#include "plugin/plugin_context.h"
#include "quickjs_lib.h"
#include "ts_host_bindings.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>

struct JSRuntime;
struct JSContext;

namespace yuan::plugin
{

    struct TsMemoryBudget
    {
        size_t used = 0;
        size_t limit = 0;
    };

    class TsScriptPluginAdapter : public ScriptPluginAdapter
    {
    public:
        struct Config
        {
            size_t memory_limit = 8 * 1024 * 1024;
            size_t max_stack_size = 1024 * 1024;
            size_t execution_timeout_ms = 5000;
        };

        explicit TsScriptPluginAdapter(const PluginManifest &manifest);
        TsScriptPluginAdapter(const PluginManifest &manifest, const Config &config);
        ~TsScriptPluginAdapter() override;

        bool load_script(const std::string &script_path) override;

        const TsMemoryBudget &memory_budget() const
        {
            if (rt_) {
                JSMemoryUsage usage;
                JS_ComputeMemoryUsage(rt_, &usage);
                memory_budget_.used = static_cast<size_t>(usage.malloc_size);
            }
            return memory_budget_;
        }

    protected:
        bool do_init(const PluginContext &context) override;
        void do_enable() override;
        void do_disable() override;
        void do_release() override;
        bool do_health_check() const override;
        void do_config_changed(const PluginConfigView &config) override;

    private:
        static int js_interrupt_handler(JSRuntime *rt, void *opaque);

        bool init_js_engine();
        void apply_sandbox();

        void call_js_void(const char *func_name);
        bool call_js_init(const PluginContext &context);
        void call_js_config_changed(const PluginConfigView &config);

        void set_execution_deadline() const;
        void clear_execution_deadline() const;
        std::string callback_owner_name() const;

        JSRuntime *rt_ = nullptr;
        JSContext *ctx_ = nullptr;
        Config config_;
        mutable TsMemoryBudget memory_budget_;
        mutable std::recursive_mutex js_mutex_;
        mutable TsInterruptData interrupt_data_;
    };

} // namespace yuan::plugin

#endif
