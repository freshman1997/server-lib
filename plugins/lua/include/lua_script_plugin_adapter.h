#ifndef __YUAN_PLUGIN_LUA_SCRIPT_PLUGIN_ADAPTER_H__
#define __YUAN_PLUGIN_LUA_SCRIPT_PLUGIN_ADAPTER_H__

#include "plugin/script_plugin_adapter.h"
#include "plugin/plugin_context.h"
#include "lua_lib.h"

#include <cstddef>
#include <mutex>
#include <string>

struct lua_State;

namespace yuan::plugin
{

    struct LuaMemoryBudget
    {
        size_t used = 0;
        size_t limit = 0;
    };

    class LuaScriptPluginAdapter : public ScriptPluginAdapter
    {
    public:
        struct Config
        {
            size_t memory_budget_bytes = 8 * 1024 * 1024;
            size_t max_instructions_per_call = 10 * 1000 * 1000;
        };

        explicit LuaScriptPluginAdapter(const PluginManifest &manifest);
        LuaScriptPluginAdapter(const PluginManifest &manifest, const Config &config);
        ~LuaScriptPluginAdapter() override;

        bool load_script(const std::string &script_path) override;

        const LuaMemoryBudget &memory_budget() const
        {
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
        static void *lua_budget_allocator(void *ud, void *ptr, size_t osize, size_t nsize);
        static void lua_execution_timeout_hook(lua_State *L, lua_Debug *ar);

        void set_execution_hook() const;
        void clear_execution_hook() const;

        bool init_lua_state();
        void apply_sandbox();

        void call_lua_void(const char *func_name);
        bool call_lua_init(const PluginContext &context);
        void call_lua_config_changed(const PluginConfigView &config);

        lua_State *L_ = nullptr;
        Config config_;
        LuaMemoryBudget memory_budget_;
        mutable std::recursive_mutex lua_mutex_;
        bool timeout_triggered_ = false;
    };

} // namespace yuan::plugin

#endif
