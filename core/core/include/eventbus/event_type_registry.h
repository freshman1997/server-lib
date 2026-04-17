#ifndef __YUAN_EVENTBUS_EVENT_TYPE_REGISTRY_H__
#define __YUAN_EVENTBUS_EVENT_TYPE_REGISTRY_H__

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace yuan::eventbus
{

    enum class EventScope 
    {
        host_internal,
        service,
        global,
    };

    struct EventDescriptor
    {
        std::string name;
        std::string category;
        std::string payload_type;
        std::string description;
        EventScope scope = EventScope::global;
    };

    class EventTypeRegistry
    {
    public:
        void register_type(EventDescriptor descriptor)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            types_[descriptor.name] = std::move(descriptor);
        }

        void register_type(std::string name, std::string category,
                           std::string payload_type, std::string description = "",
                           EventScope scope = EventScope::global)
        {
            register_type(EventDescriptor{
                std::move(name),
                std::move(category),
                std::move(payload_type),
                std::move(description),
                scope
            });
        }

        const EventDescriptor *find(const std::string &name) const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = types_.find(name);
            return it != types_.end() ? &it->second : nullptr;
        }

        bool has(const std::string &name) const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return types_.count(name) > 0;
        }

        std::vector<EventDescriptor> all() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::vector<EventDescriptor> result;
            result.reserve(types_.size());
            for (const auto & [
                                  _,
                                  desc
                              ] : types_) {
                result.push_back(desc);
            }
            return result;
        }

        std::vector<EventDescriptor> by_category(const std::string &category) const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::vector<EventDescriptor> result;
            for (const auto & [
                                  _,
                                  desc
                              ] : types_) {
                if (desc.category == category) {
                    result.push_back(desc);
                }
            }
            return result;
        }

        std::vector<EventDescriptor> by_scope(EventScope scope) const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::vector<EventDescriptor> result;
            for (const auto & [
                                  _,
                                  desc
                              ] : types_) {
                if (desc.scope == scope) {
                    result.push_back(desc);
                }
            }
            return result;
        }

        void clear()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            types_.clear();
        }

        std::size_t size() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return types_.size();
        }

    private:
        mutable std::mutex mutex_;
        std::unordered_map<std::string, EventDescriptor> types_;
    };

} // namespace yuan::eventbus

#endif
