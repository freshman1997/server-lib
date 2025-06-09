#ifndef __PLUGIN_MANAGER_H__
#define __PLUGIN_MANAGER_H__
#include "message/message.h"
#include "plugin/plugin.h"
#include "singleton/singleton.h"

#include <memory>
#include <string>

namespace yuan::plugin 
{
    class PluginManager  : public singleton::Singleton<PluginManager>, public message::MessageConsumer
    {
    public:
        PluginManager();
        ~PluginManager();

    public:
        void set_plugin_path(const std::string &path);

        bool load(const std::string &pluginName);

        Plugin * get_plugin(const std::string &name);

        void release_plugin(const std::string &pluginName);

        void message_load(const std::string &pluginName);

    public:
        virtual void on_message(const message::Message *msg);

        virtual bool need_free() { return false; }

        virtual std::set<uint32_t> get_interest_events() const;
        
    private:
        void add_plugin(const std::string &name, Plugin *plugin, void *handle);

    private:
        class PluginData;
        std::unique_ptr<PluginData> data_;
    };
}

#endif // __PLUGIN_MANAGER_H__