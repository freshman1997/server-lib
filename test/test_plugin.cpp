#include "message/message.h"
#include "message/message_dispacher.h"
#include "plugin/plugin_manager.h"
#include <any>
#include <iostream>

class PluginConsumer : public yuan::message::MessageConsumer
{
public:
    virtual void on_message(const yuan::message::Message *msg)
    {
        if (msg->type_ == yuan::message::system_message_ && msg->data_) {
            yuan::message::SystemMessage *sysmsg = static_cast<yuan::message::SystemMessage *>(msg->data_);
            const yuan::message::SystemMessage::LoadPluginResult &res = std::any_cast<const yuan::message::SystemMessage::LoadPluginResult>(sysmsg->data_);
            if (!res.result_) {
                std::cerr << "load plugin failed!! plugin name: " << res.plugin_name_ << '\n';
            }
        }
    }

    virtual bool need_free()
    {
        return true;
    }

    virtual std::set<uint32_t> get_interest_events() const
    {
        return {yuan::message::SystemMessage::load_plugin_result_};
    }
};

int main()
{
    auto pluginManager = yuan::plugin::PluginManager::get_instance();
    pluginManager->set_plugin_path("/home/yuan/code/test/server-lib/build/plugins");
    auto dispatcher = yuan::message::MessageDispatcher::get_instance();
    if (!dispatcher->init()) {
        std::cerr<< "message dispatcher init failed!!! \n";
        return -1;
    }

    if (!dispatcher->register_consumer(yuan::message::system_message_, new PluginConsumer)) {
        std::cerr << "register message consumer failed!!! \n";
        return -1;
    }

    pluginManager->message_load("HelloWorld");
    dispatcher->dispatch();

    return 0;
}