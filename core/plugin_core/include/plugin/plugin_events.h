#ifndef __YUAN_PLUGIN_PLUGIN_EVENTS_H__
#define __YUAN_PLUGIN_PLUGIN_EVENTS_H__

#include "plugin_context.h"

#include <string>

namespace yuan::plugin
{

namespace events
{
    inline constexpr const char *plugin_loaded = "plugin.loaded";
    inline constexpr const char *plugin_load_failed = "plugin.load_failed";
    inline constexpr const char *plugin_unloading = "plugin.unloading";
    inline constexpr const char *plugin_unloaded = "plugin.unloaded";
    inline constexpr const char *plugin_health_checked = "plugin.health_checked";
    inline constexpr const char *plugin_config_changed = "plugin.config_changed";
    inline constexpr const char *plugin_service_registered = "plugin.service_registered";
    inline constexpr const char *plugin_service_unregistered = "plugin.service_unregistered";
}

struct PluginEventDescriptor
{
    const char *name = "";
    const char *payload_type = "";
};

namespace event_descriptors
{
    inline constexpr PluginEventDescriptor loaded{events::plugin_loaded, "PluginEvent"};
    inline constexpr PluginEventDescriptor load_failed{events::plugin_load_failed, "PluginLoadFailedEvent"};
    inline constexpr PluginEventDescriptor unloading{events::plugin_unloading, "PluginEvent"};
    inline constexpr PluginEventDescriptor unloaded{events::plugin_unloaded, "PluginEvent"};
    inline constexpr PluginEventDescriptor health_checked{events::plugin_health_checked, "PluginHealthCheckEvent"};
    inline constexpr PluginEventDescriptor config_changed{events::plugin_config_changed, "PluginConfigChangedEvent"};
    inline constexpr PluginEventDescriptor service_registered{events::plugin_service_registered, "PluginServiceEvent"};
    inline constexpr PluginEventDescriptor service_unregistered{events::plugin_service_unregistered, "PluginServiceEvent"};
}

struct PluginEvent
{
    std::string app_name;
    std::string plugin_name;
    PluginRunMode run_mode = PluginRunMode::unknown;
    std::size_t worker_threads = 1;
    std::size_t worker_index = 0;
    bool is_worker_process = false;
};

struct PluginLoadFailedEvent : public PluginEvent
{
    std::string reason;
};

struct PluginHealthCheckEvent : public PluginEvent
{
    bool healthy = false;
};

struct PluginConfigChangedEvent : public PluginEvent
{
    std::string config_path;
};

struct PluginServiceEvent : public PluginEvent
{
    std::string service_name;
    std::string type_name;
};

} // namespace yuan::plugin

#endif
