#include "helloworld.h"
#include "api/api.h"
#include "singleton/singleton.h"

#include <iostream>

HelloWorldPlugin::HelloWorldPlugin()
{

}

HelloWorldPlugin::~HelloWorldPlugin()
{

}

void HelloWorldPlugin::on_loaded()
{
    std::cout << "hello world on_loaded !!\n";
}

bool HelloWorldPlugin::on_init()
{
    std::cout << "hello world init success !!\n";
    return true;
}

int HelloWorldPlugin::on_message(const std::string &message)
{
    std::cout << "hello world on_message: " << message << "\n";
    return 0;
}

void HelloWorldPlugin::on_release()
{
    std::cout << "hello world on_release !!\n";
}

YUAN_API_C_EXPORT void * get_HelloWorld_plugin_instance()
{
    return yuan::singleton::get_instance<HelloWorldPlugin>();
}