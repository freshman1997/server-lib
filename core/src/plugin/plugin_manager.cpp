#include "plugin/plugin_manager.h"
#include "message/message.h"
#include "plugin/plugin.h"
#include "plugin/plugin_symbol_solver.h"
#include "message/message_dispacher.h"

#include <iostream>
#include <unordered_map>

namespace yuan::plugin 
{
    typedef void * (*plugin_entry_function)(void);

    class PluginManager::PluginData
    {
    public:
        PluginData() = default;
        ~PluginData()
        {
            for (auto &item : plugins_) {
                item.second->on_release();
            }
            plugins_.clear();
        }

    public:
        std::string plugin_path_;
        std::unordered_map<std::string, Plugin *> plugins_;
    };

    PluginManager::PluginManager() : data_(std::make_unique<PluginManager::PluginData>())
    {
        message::MessageDispatcher::get_instance()->register_consumer(message::system_message_, this);
    }

    PluginManager::~PluginManager()
    {
    }

    void PluginManager::set_plugin_path(const std::string &path)
    {
        data_->plugin_path_ = path;
        if (!path.empty() && !path.ends_with('/'))
        {
            data_->plugin_path_.push_back('/');
        }
    }

    bool PluginManager::load(const std::string &pluginName)
    {
        std::string realName = data_->plugin_path_ + pluginName + ".plugin";
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

        add_plugin(pluginName, plugin);

        if (!plugin->on_init(yuan::message::MessageDispatcher::get_instance())) {
            std::cout << "plugin " << pluginName << " init failed!!!\n";
            release_plugin(pluginName);
            return false;
        }

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

    Plugin * PluginManager::get_plugin(const std::string &name)
    {
        auto it = data_->plugins_.find(name);
        return it == data_->plugins_.end() ? nullptr : it->second;
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

    void PluginManager::on_message(const message::Message *msg)
    {
        if (msg->type_ == message::system_message_ && msg->data_) {
            const message::SystemMessage *sysmsg = static_cast<const message::SystemMessage *>(msg->data_);
            if (!sysmsg->plugin_name_.empty()) {
                switch (msg->event_) {
                case message::SystemMessage::SystemMessageType::load_plugin_:
                {
                    load(sysmsg->plugin_name_);
                    break;
                }
                case message::SystemMessage::SystemMessageType::release_plugin_:
                {
                    release_plugin(sysmsg->plugin_name_);
                    break;
                }
                default: {
                    std::cerr << "cant handle plugin message: " << msg->event_ << " !!\n";
                    break;
                }
                }
            }
        }
    }
}
