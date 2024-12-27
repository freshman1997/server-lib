#ifndef __PLUGIN_MANAGER_H__
#define __PLUGIN_MANAGER_H__
#include "plugin/plugin.h"
#include <memory>
#include <string>

namespace yuan::plugin 
{
    class PluginManager
    {
    public:
        PluginManager();
        ~PluginManager();

    public:
        bool load(const std::string &pluginName);

        void add_plugin(const std::string &name, Plugin *plugin);

        void release_plugin(const std::string &pluginName);

    private:
        class PluginData;
        std::unique_ptr<PluginData> data_;
    };
}

#endif // __PLUGIN_MANAGER_H__