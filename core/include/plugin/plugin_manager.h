#ifndef __PLUGIN_MANAGER_H__
#define __PLUGIN_MANAGER_H__
#include "plugin/plugin.h"
#include "singleton/singleton.h"
#include <string>

namespace yuan::plugin 
{
    class PluginManager : singleton::Singleton<PluginManager>
    {
    public:
        void * load(const std::string &pluginPath);

        void add_plugin(Plugin *plugin);

        void release_plugin(Plugin *plugin);
    };
}

#endif // __PLUGIN_MANAGER_H__