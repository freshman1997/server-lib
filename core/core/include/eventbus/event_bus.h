#ifndef __YUAN_EVENTBUS_EVENT_BUS_H__
#define __YUAN_EVENTBUS_EVENT_BUS_H__

#include <any>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace yuan::eventbus
{

struct Event
{
    std::string name;
    std::any payload;
};

using EventHandler = std::function<void(const Event&)>;
using SubscriptionToken = std::uint64_t;

class EventBus
{
public:
    SubscriptionToken subscribe(const std::string& event_name, EventHandler handler);
    bool unsubscribe(SubscriptionToken token);
    void publish(const Event& event) const;
    void publish(std::string event_name, std::any payload = {}) const;

private:
    mutable std::mutex mutex_;
    mutable SubscriptionToken next_token_ = 1;
    std::unordered_map<std::string, std::unordered_map<SubscriptionToken, EventHandler>> handlers_;
    std::unordered_map<SubscriptionToken, std::string> token_index_;
};

} // namespace yuan::eventbus

#endif
