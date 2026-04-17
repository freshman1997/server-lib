#ifndef __YUAN_PLUGIN_SCRIPT_PLUGIN_ADAPTER_H__
#define __YUAN_PLUGIN_SCRIPT_PLUGIN_ADAPTER_H__

#include "plugin/plugin.h"
#include "plugin/plugin_manifest.h"

#include <string>

namespace yuan::plugin
{

    class ScriptPluginAdapter : public Plugin
    {
    public:
        explicit ScriptPluginAdapter(const PluginManifest &manifest);
        ~ScriptPluginAdapter() override;

        void on_loaded() override;
        bool on_init(const PluginContext &context) override;
        void on_enable() override;
        void on_disable() override;
        void on_release() override;
        bool on_health_check() const override;
        void on_config_changed(const PluginConfigView &config) override;
        PluginMeta meta() const override;
        PluginManifest manifest() const override;

        virtual bool load_script(const std::string &script_path) = 0;

    protected:
        virtual bool do_init(const PluginContext &context) = 0;
        virtual void do_enable()
        {
        }
        virtual void do_disable()
        {
        }
        virtual void do_release()
        {
        }
        virtual bool do_health_check() const
        {
            return true;
        }
        virtual void do_config_changed(const PluginConfigView & /*config*/)
        {
        }

        PluginManifest manifest_;
        PluginContext context_;
        bool script_loaded_ = false;
    };

} // namespace yuan::plugin

#endif
