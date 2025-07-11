#include "plugin/plugin_manager.h"
#include "message/message.h"
#include "plugin/plugin.h"
#include "plugin/plugin_symbol_solver.h"
#include "message/message_dispacher.h"

#include <any>
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
                item.second.second->on_release();
                PluginSymbolSolver::release_native_lib(item.second.first);
            }
            plugins_.clear();
        }

    public:
        std::string plugin_path_;
        std::unordered_map<std::string, std::pair<void *, Plugin *>> plugins_;
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

        add_plugin(pluginName, plugin, handle);

        if (!plugin->on_init(yuan::message::MessageDispatcher::get_instance())) {
            std::cout << "plugin " << pluginName << " init failed!!!\n";
            release_plugin(pluginName);
            return false;
        }

        return true;
    }

    void PluginManager::add_plugin(const std::string &name, Plugin *plugin, void *handle)
    {
        auto it = data_->plugins_.find(name);
        if (it != data_->plugins_.end()) {
            release_plugin(name);
        }
        data_->plugins_[name] = {handle, plugin};
    }

    Plugin * PluginManager::get_plugin(const std::string &name)
    {
        auto it = data_->plugins_.find(name);
        return it == data_->plugins_.end() ? nullptr : it->second.second;
    }

    void PluginManager::release_plugin(const std::string &pluginName)
    {
        auto it = data_->plugins_.find(pluginName);
        if (it == data_->plugins_.end()) {
            return;
        }

        auto plugin = it->second.second;
        if (plugin)
        {
            plugin->on_release();
        }

        PluginSymbolSolver::release_native_lib(it->second.first);

        data_->plugins_.erase(it);
    }

    void PluginManager::async_load(const std::string &pluginName)
    {
        message::Message *msg = new message::Message;
        msg->type_ = message::system_message_;
        msg->event_id_ = message::SystemMessage::load_plugin_;
        message::SystemMessage *sysMsg = new message::SystemMessage;
        sysMsg->data_ = pluginName;
        msg->data_ = sysMsg;

        message::MessageDispatcher::get_instance()->send_message(msg);
    }

    void PluginManager::on_message(const message::Message *msg)
    {
        if (msg->type_ == message::system_message_ && msg->data_) {
            const message::SystemMessage *sysmsg = static_cast<const message::SystemMessage *>(msg->data_);
            if (sysmsg) {
                switch (msg->event_id_) {
                case message::SystemMessage::SystemMessageType::load_plugin_:
                {
                    const auto pluginName = std::any_cast<std::string>(sysmsg->data_);
                    bool res = load(pluginName);

                    message::Message *msg = new message::Message;
                    msg->type_ = message::system_message_;
                    msg->event_id_ = message::SystemMessage::load_plugin_result_;
                    message::SystemMessage *sysMsg = new message::SystemMessage;
                    sysMsg->data_ = message::SystemMessage::LoadPluginResult{pluginName, res};
                    msg->data_ = sysMsg;

                    message::MessageDispatcher::get_instance()->send_message(msg);
                    break;
                }
                case message::SystemMessage::SystemMessageType::release_plugin_:
                {
                    const auto pluginName = std::any_cast<std::string>(sysmsg->data_);
                    release_plugin(pluginName);
                    break;
                }
                default: {
                    std::cerr << "cant handle plugin message: " << msg->event_id_ << " !!\n";
                    break;
                }
                }
            }
        }
    }

    std::set<uint32_t> PluginManager::get_interest_events() const
    {
        return {message::SystemMessage::SystemMessageType::load_plugin_, message::SystemMessage::SystemMessageType::release_plugin_};
    }
}
