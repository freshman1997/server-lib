#ifndef __YUAN_APP_PLUGIN_EVENT_BUS_ADAPTER_H__
#define __YUAN_APP_PLUGIN_EVENT_BUS_ADAPTER_H__

#include "plugin/host_event_bus.h"
#include "plugin/host_resource_guard.h"

#include <memory>
#include <string>

namespace yuan::eventbus
{
class EventBus;
}

namespace yuan::app
{

class PluginEventBusAdapter : public plugin::HostEventBus
{
public:
    explicit PluginEventBusAdapter(std::shared_ptr<eventbus::EventBus> event_bus);

    plugin::HostEventSubscription subscribe(const std::string &event_name, plugin::HostEventHandler handler) override;
    bool unsubscribe(plugin::HostEventSubscription token) override;
    void publish(std::string event_name, std::any payload = {}) override;

    /// 设置当前插件的名称和资源守卫 (用于自动追踪事件订阅)
    void set_plugin_context(const std::string &plugin_name, plugin::HostResourceGuard *guard);

private:
    std::shared_ptr<eventbus::EventBus> event_bus_;
    std::string plugin_name_;
    plugin::HostResourceGuard *resource_guard_ = nullptr;
};

} // namespace yuan::app

#endif
