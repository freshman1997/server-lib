#include "plugin/script_plugin_registry.h"
#include "logger.h"

namespace yuan::plugin
{

    ScriptPluginRegistry &ScriptPluginRegistry::instance()
    {
        static ScriptPluginRegistry reg;
        return reg;
    }

    void ScriptPluginRegistry::register_adapter(const std::string & language, FactoryFn factory)
    {
        factories_[language] = std::move(factory);
        LOG_INFO("script plugin adapter registered for language '{}'", language);
    }

    ScriptPluginAdapter *ScriptPluginRegistry::create(const std::string & language,
                                                      const PluginManifest & manifest,
                                                      const PluginConfigView & config) const
    {
        auto it = factories_.find(language);
        if (it == factories_.end()) {
            LOG_ERROR("no script adapter registered for language '{}'", language);
            return nullptr;
        }
        return it->second(manifest, config);
    }

    bool ScriptPluginRegistry::has_adapter(const std::string & language) const
    {
        return factories_.count(language) > 0;
    }

    std::vector<std::string> ScriptPluginRegistry::available_languages() const
    {
        std::vector<std::string> languages;
        languages.reserve(factories_.size());
        for (const auto & [
                              lang,
                              _
                          ] : factories_) {
            languages.push_back(lang);
        }
        return languages;
    }

} // namespace yuan::plugin
