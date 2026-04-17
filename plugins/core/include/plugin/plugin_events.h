#ifndef __YUAN_PLUGIN_PLUGIN_EVENTS_H__
#define __YUAN_PLUGIN_PLUGIN_EVENTS_H__

#include "plugin/plugin_context.h"
#include "plugin/plugin_manifest.h"
#include "plugin/plugin_permission.h"

#include <cstddef>
#include <string>

namespace yuan::plugin
{

    enum class EventScope {
        host_internal,
        plugin_local,
        global,
    };

    enum class EventDeliverySemantics {
        async,
        sync,
    };

    namespace events
    {
        inline constexpr const char *plugin_discovered = "plugin.discovered";
        inline constexpr const char *plugin_loaded = "plugin.loaded";
        inline constexpr const char *plugin_load_failed = "plugin.load_failed";
        inline constexpr const char *plugin_initialized = "plugin.initialized";
        inline constexpr const char *plugin_activated = "plugin.activated";
        inline constexpr const char *plugin_degraded = "plugin.degraded";
        inline constexpr const char *plugin_faulted = "plugin.faulted";
        inline constexpr const char *plugin_quarantined = "plugin.quarantined";
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
        const char *category = "";
        const char *payload_type = "";
        EventScope scope = EventScope::global;
        EventDeliverySemantics delivery_semantics = EventDeliverySemantics::async;
        PluginPermission required_permission = PluginPermission::none;
    };

    namespace event_descriptors
    {
        inline constexpr PluginEventDescriptor discovered{
            events::plugin_discovered, "lifecycle", "PluginEvent",
            EventScope::host_internal, EventDeliverySemantics::sync, PluginPermission::none
        };

        inline constexpr PluginEventDescriptor loaded{
            events::plugin_loaded, "lifecycle", "PluginEvent",
            EventScope::global, EventDeliverySemantics::sync, PluginPermission::none
        };

        inline constexpr PluginEventDescriptor load_failed{
            events::plugin_load_failed, "lifecycle", "PluginLoadFailedEvent",
            EventScope::global, EventDeliverySemantics::sync, PluginPermission::none
        };

        inline constexpr PluginEventDescriptor initialized{
            events::plugin_initialized, "lifecycle", "PluginEvent",
            EventScope::global, EventDeliverySemantics::async, PluginPermission::none
        };

        inline constexpr PluginEventDescriptor activated{
            events::plugin_activated, "lifecycle", "PluginEvent",
            EventScope::global, EventDeliverySemantics::async, PluginPermission::none
        };

        inline constexpr PluginEventDescriptor degraded{
            events::plugin_degraded, "lifecycle", "PluginEvent",
            EventScope::global, EventDeliverySemantics::async, PluginPermission::none
        };

        inline constexpr PluginEventDescriptor faulted{
            events::plugin_faulted, "lifecycle", "PluginFaultEvent",
            EventScope::global, EventDeliverySemantics::sync, PluginPermission::none
        };

        inline constexpr PluginEventDescriptor quarantined{
            events::plugin_quarantined, "lifecycle", "PluginEvent",
            EventScope::global, EventDeliverySemantics::sync, PluginPermission::none
        };

        inline constexpr PluginEventDescriptor unloading{
            events::plugin_unloading, "lifecycle", "PluginEvent",
            EventScope::global, EventDeliverySemantics::sync, PluginPermission::none
        };

        inline constexpr PluginEventDescriptor unloaded{
            events::plugin_unloaded, "lifecycle", "PluginEvent",
            EventScope::global, EventDeliverySemantics::sync, PluginPermission::none
        };

        inline constexpr PluginEventDescriptor health_checked{
            events::plugin_health_checked, "domain", "PluginHealthCheckEvent",
            EventScope::global, EventDeliverySemantics::async, PluginPermission::none
        };

        inline constexpr PluginEventDescriptor config_changed{
            events::plugin_config_changed, "domain", "PluginConfigChangedEvent",
            EventScope::plugin_local, EventDeliverySemantics::async, PluginPermission::none
        };

        inline constexpr PluginEventDescriptor service_registered{
            events::plugin_service_registered, "domain", "PluginServiceEvent",
            EventScope::global, EventDeliverySemantics::async, PluginPermission::use_service_registry
        };

        inline constexpr PluginEventDescriptor service_unregistered{
            events::plugin_service_unregistered, "domain", "PluginServiceEvent",
            EventScope::global, EventDeliverySemantics::async, PluginPermission::use_service_registry
        };
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

    struct PluginFaultEvent : public PluginEvent
    {
        std::string fault_message;
        std::string call_site;
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
