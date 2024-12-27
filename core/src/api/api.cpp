#include "api/api.h"
#include "plugin/plugin_manager.h"
#include "singleton/singleton.h"

void * get_plugin_manager()
{
    return yuan::singleton::get_instance<yuan::plugin::PluginManager>();
}