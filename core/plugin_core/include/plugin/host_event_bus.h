#ifndef __YUAN_PLUGIN_HOST_EVENT_BUS_H__
#define __YUAN_PLUGIN_HOST_EVENT_BUS_H__

#include <any>
#include <cstdint>
#include <functional>
#include <string>

namespace yuan::plugin
{

using HostEventSubscription = std::uint64_t;

struct HostEvent
{
    std::string name;
    std::any payload;
};

using HostEventHandler = std::function<void(const HostEvent &)>;

class HostEventBus
{
public:
    virtual ~HostEventBus() = default;

    virtual HostEventSubscription subscribe(const std::string &event_name, HostEventHandler handler) = 0;
    virtual bool unsubscribe(HostEventSubscription token) = 0;
    virtual void publish(std::string event_name, std::any payload = {}) = 0;
};

} // namespace yuan::plugin

#endif
