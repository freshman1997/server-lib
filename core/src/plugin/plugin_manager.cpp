#include "plugin/plugin_manager.h"
#include "plugin/plugin.h"
#include "plugin/plugin_symbol_solver.h"

#include <iostream>
#include <unordered_map>

namespace yuan::plugin 
{
    typedef void * (*plugin_entry_function)(void);

    class PluginManager::PluginData
    {
    public:
        ~PluginData()
        {
            for (auto &item : plugins_) {
                item.second->on_release();
            }
            plugins_.clear();
        }
    public:
        std::unordered_map<std::string, Plugin *> plugins_;
    };

    PluginManager::PluginManager() : data_(std::make_unique<PluginManager::PluginData>())
    {
    }

    PluginManager::~PluginManager()
    {
    }

    bool PluginManager::load(const std::string &pluginName)
    {
        std::string realName = "/home/yuan/codes/test/server-lib/build/plugins/" + pluginName + ".so";
        void *handle = PluginSymbolSolver::load_native_lib(realName);
        if (!handle) {
            return false;
        }

        std::string entrySymbol = "get_" + pluginName + "_plugin_instance";

        plugin_entry_function func = nullptr;
        *(void **)(&func) = PluginSymbolSolver::find_symbol(handle, entrySymbol);
        if (!func) {
            return false;
        }

        void * pluginInstance = func();
        if (!pluginInstance) {
            return false;
        }

        std::cout << "load " << pluginName << " success!\n";

        Plugin *plugin = static_cast<Plugin *>(pluginInstance);
        plugin->on_loaded();

        if (!plugin->on_init()) {
            std::cout << "plugin " << pluginName << " init failed!!!\n";
            return false;
        }

        add_plugin(pluginName, plugin);

        return true;
    }

    void PluginManager::add_plugin(const std::string &name, Plugin *plugin)
    {
        auto it = data_->plugins_.find(name);
        if (it != data_->plugins_.end()) {
            release_plugin(name);
        }
        data_->plugins_[name] = plugin;
    }

    void PluginManager::release_plugin(const std::string &pluginName)
    {
        auto it = data_->plugins_.find(pluginName);
        if (it == data_->plugins_.end()) {
            return;
        }

        auto plugin = it->second;
        if (plugin)
        {
            plugin->on_release();
        }

        data_->plugins_.erase(it);
    }
}
