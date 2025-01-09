#include "message/message_dispacher.h"
#include "plugin/plugin_manager.h"

int main()
{
    auto pluginManager = yuan::plugin::PluginManager::get_instance();
    pluginManager->set_plugin_path("/home/yuan/code/test/server-lib/build/plugins");

    if (!pluginManager->load("HelloWorld")) {
        return -1;
    }

    auto dispatcher = yuan::message::MessageDispatcher::get_instance();
    dispatcher->dispatch();

    return 0;
}