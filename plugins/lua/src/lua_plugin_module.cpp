#include "lua_plugin_module.h"
#include "lua_script_plugin_adapter.h"
#include "plugin/script_plugin_registry.h"
#include "logger.h"

namespace yuan::plugin
{

    void init_lua_plugin_module()
    {
        ScriptPluginRegistry::instance().register_adapter("lua",
                                                          [](const PluginManifest & manifest,
                                                             const PluginConfigView & config)->ScriptPluginAdapter *
        {
                                                              LuaScriptPluginAdapter::Config lua_config;
                                                              lua_config.memory_budget_bytes = static_cast<size_t>(
                                                                  config.get_int64("lua_memory_budget_bytes", 8 * 1024 * 1024));
                                                              lua_config.max_instructions_per_call = static_cast<size_t>(
                                                                  config.get_int64("lua_max_instructions", 10 * 1000 * 1000));

                                                              return new LuaScriptPluginAdapter(manifest, lua_config);
        });
    }

} // namespace yuan::plugin
