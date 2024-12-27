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

    return 0;
}