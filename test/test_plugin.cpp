#include "plugin/plugin_manager.h"
#include "singleton/singleton.h"
#include <iostream>

int main()
{
    std::cout << "hello world!!!\n";
    auto pluginManager = yuan::singleton::get_instance<yuan::plugin::PluginManager>();
    if (!pluginManager->load("HelloWorld")) {
        return -1;
    }

    if (auto plugin = pluginManager->get_plugin("HelloWorld")) {
        plugin->on_message("test message");
    }

    return 0;
}