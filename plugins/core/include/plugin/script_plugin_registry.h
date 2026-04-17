#ifndef __YUAN_PLUGIN_SCRIPT_PLUGIN_REGISTRY_H__
#define __YUAN_PLUGIN_SCRIPT_PLUGIN_REGISTRY_H__

#include "plugin/script_plugin_adapter.h"
#include "plugin/plugin_config_view.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::plugin
{

    class ScriptPluginRegistry
    {
    public:
        using FactoryFn = std::function<ScriptPluginAdapter *(const PluginManifest &, const PluginConfigView &)>;

        static ScriptPluginRegistry &instance();

        void register_adapter(const std::string &language, FactoryFn factory);

        ScriptPluginAdapter *create(const std::string &language,
                                    const PluginManifest &manifest,
                                    const PluginConfigView &config) const;

        bool has_adapter(const std::string &language) const;

        std::vector<std::string> available_languages() const;

    private:
        ScriptPluginRegistry() = default;
        std::unordered_map<std::string, FactoryFn> factories_;
    };

} // namespace yuan::plugin

#endif
