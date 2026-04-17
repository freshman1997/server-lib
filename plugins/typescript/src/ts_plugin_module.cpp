#include "ts_plugin_module.h"
#include "ts_script_plugin_adapter.h"
#include "plugin/script_plugin_registry.h"
#include "logger.h"

namespace yuan::plugin
{

    void init_ts_plugin_module()
    {
        ScriptPluginRegistry::instance().register_adapter("typescript",
                                                          [](const PluginManifest & manifest,
                                                             const PluginConfigView & config)->ScriptPluginAdapter *
        {
                                                              TsScriptPluginAdapter::Config ts_config;
                                                              ts_config.memory_limit = static_cast<size_t>(
                                                                  config.get_int64("ts_memory_limit", 8 * 1024 * 1024));
                                                              ts_config.max_stack_size = static_cast<size_t>(
                                                                  config.get_int64("ts_max_stack_size", 1024 * 1024));
                                                              ts_config.execution_timeout_ms = static_cast<size_t>(
                                                                  config.get_int64("ts_execution_timeout_ms", 5000));

                                                              return new TsScriptPluginAdapter(manifest, ts_config);
        });
    }

} // namespace yuan::plugin
