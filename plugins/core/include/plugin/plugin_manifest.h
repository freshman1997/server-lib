#ifndef __YUAN_PLUGIN_PLUGIN_MANIFEST_H__
#define __YUAN_PLUGIN_PLUGIN_MANIFEST_H__

#include "plugin/plugin_permission.h"

#include <string>
#include <vector>

namespace yuan::plugin
{

    struct ExtensionPointDescriptor
    {
        std::string name;
        std::string type;
        std::string contract_id;
        int contract_version = 1;
    };

    enum class PluginRunMode {
        unknown,
        single_thread,
        multi_thread,
        multi_process,
        sandbox,
        script,
    };

    struct ProtocolServiceDescriptor
    {
        std::string plugin_id;
        std::string name;
        std::string type;
        std::string protocol = "tcp";
        std::string transport = "tcp";
        std::string host = "0.0.0.0";
        int port = 0;
        std::string handler;
        std::string framing = "raw";
        int read_timeout_ms = 30000;
        int idle_timeout_ms = 60000;
        int write_timeout_ms = 30000;
        int max_connections = 1024;
        int max_frame_bytes = 65536;
        int max_write_buffer_bytes = 1048576;
        std::string contract_id;
        int contract_version = 1;
        PluginRunMode run_mode = PluginRunMode::unknown;
        std::string language;
        std::string entry;
    };

    struct PluginManifest
    {
        std::string plugin_id;
        std::string name;
        std::string version = "1.0.0";
        std::string author;
        std::string description;
        int api_version = 1;
        std::string entry;
        PluginPermission required_permissions = PluginPermission::none;
        std::vector<std::string> depends_on;
        std::vector<ExtensionPointDescriptor> extension_points;
        std::vector<ProtocolServiceDescriptor> protocol_services;
        PluginRunMode run_mode = PluginRunMode::unknown;
        std::string language;
    };

} // namespace yuan::plugin

#endif
