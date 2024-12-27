#include "helloworld.h"
#include "api/api.h"
#include "plugin/plugin_manager.h"
#include "singleton/singleton.h"
#include <iostream>

HelloWorldPlugin::HelloWorldPlugin() : plugin_manager_(nullptr)
{

}

HelloWorldPlugin::~HelloWorldPlugin()
{

}

void HelloWorldPlugin::on_loaded()
{
    void * pluginManager = get_plugin_manager();
    if (!pluginManager)
    {
        std::cout << "invalid state, plugin manager is null !!!\n";
        return;
    }

    plugin_manager_ = static_cast<yuan::plugin::PluginManager *>(pluginManager);
}

bool HelloWorldPlugin::on_init()
{
    std::cout << "hello world init success !!\n";
    return true;
}

void HelloWorldPlugin::on_release()
{
    std::cout << "hello world on_release !!\n";
}

YUAN_API_C_EXPORT void * get_HelloWorld_plugin_instance()
{
    return yuan::singleton::get_instance<HelloWorldPlugin>();
}