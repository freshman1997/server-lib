#include "plugin/script_plugin_adapter.h"

namespace yuan::plugin
{

    ScriptPluginAdapter::ScriptPluginAdapter(const PluginManifest & manifest)
        : manifest_(manifest)
    {
    }

    ScriptPluginAdapter::~ScriptPluginAdapter() = default;

    void ScriptPluginAdapter::on_loaded()
    {
    }

    bool ScriptPluginAdapter::on_init(const PluginContext & context)
    {
        if (!script_loaded_) {
            return false;
        }
        context_ = context;
        return do_init(context_);
    }

    void ScriptPluginAdapter::on_enable()
    {
        if (script_loaded_) {
            do_enable();
        }
    }

    void ScriptPluginAdapter::on_disable()
    {
        if (script_loaded_) {
            do_disable();
        }
    }

    void ScriptPluginAdapter::on_release()
    {
        if (script_loaded_) {
            do_release();
        }
    }

    bool ScriptPluginAdapter::on_health_check() const
    {
        if (!script_loaded_) {
            return true;
        }
        return do_health_check();
    }

    void ScriptPluginAdapter::on_config_changed(const PluginConfigView & config)
    {
        do_config_changed(config);
    }

    PluginMeta ScriptPluginAdapter::meta() const
    {
        PluginMeta m;
        m.name = manifest_.name;
        m.version = manifest_.version;
        m.author = manifest_.author;
        m.description = manifest_.description;
        m.api_version = manifest_.api_version;
        m.required_permissions = manifest_.required_permissions;
        m.depends_on = manifest_.depends_on;
        return m;
    }

    PluginManifest ScriptPluginAdapter::manifest() const
    {
        return manifest_;
    }

} // namespace yuan::plugin
