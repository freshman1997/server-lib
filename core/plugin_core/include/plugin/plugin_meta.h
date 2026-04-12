#ifndef __YUAN_PLUGIN_PLUGIN_META_H__
#define __YUAN_PLUGIN_PLUGIN_META_H__

#include "plugin/plugin_permission.h"

#include <string>
#include <vector>

namespace yuan::plugin
{

struct PluginMeta
{
    std::string name;
    std::string version = "1.0.0";
    std::string author;
    std::string description;
    int api_version = 1;
    std::vector<std::string> depends_on;

    /// 插件声明需要的权限 (运行时由宿主审核授予)
    PluginPermission required_permissions = PluginPermission::all;
};

} // namespace yuan::plugin

#endif
