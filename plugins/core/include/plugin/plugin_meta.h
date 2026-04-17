#ifndef __YUAN_PLUGIN_PLUGIN_META_H__
#define __YUAN_PLUGIN_PLUGIN_META_H__

#include "plugin/plugin_manifest.h"
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

        PluginPermission required_permissions = PluginPermission::none;

        PluginManifest to_manifest() const
        {
            PluginManifest manifest;
            manifest.plugin_id = name;
            manifest.name = name;
            manifest.version = version;
            manifest.author = author;
            manifest.description = description;
            manifest.api_version = api_version;
            manifest.depends_on = depends_on;
            manifest.required_permissions = required_permissions;
            return manifest;
        }
    };

} // namespace yuan::plugin

#endif
