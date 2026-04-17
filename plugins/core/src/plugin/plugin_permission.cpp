#include "plugin/plugin_permission.h"

#include <sstream>
#include <algorithm>

namespace yuan::plugin
{

    PluginPermission PluginPermissionNames::parse(const std::string & str)
    {
        if (str.empty()) {
            return PluginPermission::none;
        }

        // 支持特殊值 "all" / "none"
        if (str == "all") {
            return PluginPermission::all;
        }
        if (str == "none") {
            return PluginPermission::none;
        }

        PluginPermission result = PluginPermission::none;
        std::istringstream iss(str);
        std::string token;
        while (std::getline(iss, token, ',')) {
            // trim whitespace
            size_t start = token.find_first_not_of(" \t");
            size_t end = token.find_last_not_of(" \t");
            if (start == std::string::npos)
                continue;
            token = token.substr(start, end - start + 1);

            if (token == "use_event_bus")
                result = result | PluginPermission::use_event_bus;
            else if (token == "use_logger")
                result = result | PluginPermission::use_logger;
            else if (token == "use_scheduler")
                result = result | PluginPermission::use_scheduler;
            else if (token == "use_service_catalog")
                result = result | PluginPermission::use_service_catalog;
            else if (token == "use_service_registry")
                result = result | PluginPermission::use_service_registry;
            else if (token == "use_http_intercept")
                result = result | PluginPermission::use_http_intercept;
            else if (token == "use_storage")
                result = result | PluginPermission::use_storage;
            else if (token == "use_network_runtime")
                result = result | PluginPermission::use_network_runtime;
            else if (token == "use_extension_points")
                result = result | PluginPermission::use_extension_points;
            else if (token == "all")
                result = result | PluginPermission::all;
        }
        return result;
    }

    std::vector<std::string> PluginPermissionNames::to_names(PluginPermission p)
    {
        std::vector<std::string> names;
        auto check = [&](PluginPermission bit, const char *n) {
        if (has_permission(p, bit)) names.push_back(n);
        };
        check(PluginPermission::use_event_bus, "use_event_bus");
        check(PluginPermission::use_logger, "use_logger");
        check(PluginPermission::use_scheduler, "use_scheduler");
        check(PluginPermission::use_service_catalog, "use_service_catalog");
        check(PluginPermission::use_service_registry, "use_service_registry");
        check(PluginPermission::use_http_intercept, "use_http_intercept");
        check(PluginPermission::use_storage, "use_storage");
        check(PluginPermission::use_network_runtime, "use_network_runtime");
        check(PluginPermission::use_extension_points, "use_extension_points");
        return names;
    }

} // namespace yuan::plugin
