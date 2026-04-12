#include "eventbus/event_bus.h"

#include <utility>
#include <vector>

namespace yuan::eventbus
{

SubscriptionToken EventBus::subscribe(const std::string& event_name, EventHandler handler)
{
    if (event_name.empty() || !handler) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto token = next_token_++;
    handlers_[event_name][token] = std::move(handler);
    token_index_[token] = event_name;
    return token;
}

bool EventBus::unsubscribe(SubscriptionToken token)
{
    if (token == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto token_it = token_index_.find(token);
    if (token_it == token_index_.end()) {
        return false;
    }

    const auto handlers_it = handlers_.find(token_it->second);
    if (handlers_it == handlers_.end()) {
        token_index_.erase(token_it);
        return false;
    }

    const auto erased = handlers_it->second.erase(token);
    if (handlers_it->second.empty()) {
        handlers_.erase(handlers_it);
    }
    token_index_.erase(token_it);
    return erased > 0;
}

void EventBus::publish(const Event& event) const
{
    if (event.name.empty()) {
        return;
    }

    std::vector<EventHandler> handlers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = handlers_.find(event.name);
        if (it == handlers_.end()) {
            return;
        }

        handlers.reserve(it->second.size());
        for (const auto& handlerEntry : it->second) {
            if (handlerEntry.second) {
                handlers.push_back(handlerEntry.second);
            }
        }
    }

    for (const auto& handler : handlers) {
        handler(event);
    }
}

void EventBus::publish(std::string event_name, std::any payload) const
{
    publish(Event{std::move(event_name), std::move(payload)});
}

} // namespace yuan::eventbus
