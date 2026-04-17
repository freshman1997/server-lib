#include "plugin_event_bus_adapter.h"

#include "eventbus/event_bus.h"

namespace yuan::app
{

    PluginEventBusAdapter::PluginEventBusAdapter(std::shared_ptr<eventbus::EventBus> event_bus)
        : event_bus_(std::move(event_bus))
    {
    }

    void PluginEventBusAdapter::set_plugin_context(const std::string & plugin_name, plugin::HostResourceGuard * guard)
    {
        // no-op — resource tracking moved to PluginContextHelper::track_resource()
        (void)plugin_name;
        (void)guard;
    }

    plugin::HostEventSubscription PluginEventBusAdapter::subscribe(
        const std::string & event_name,
        plugin::HostEventHandler handler)
    {
        if (!event_bus_ || !handler) {
            return 0;
        }

        return event_bus_->subscribe(event_name, [handler = std::move(handler)](const eventbus::Event & event) {
        handler(plugin::HostEvent{event.name, event.payload});
        });
    }

    bool PluginEventBusAdapter::unsubscribe(plugin::HostEventSubscription token)
    {
        return event_bus_ && event_bus_->unsubscribe(token);
    }

    void PluginEventBusAdapter::publish(std::string event_name, std::any payload)
    {
        if (event_bus_) {
            event_bus_->publish(std::move(event_name), std::move(payload));
        }
    }

} // namespace yuan::app
