#ifndef YUAN_GAME_BASE_ENTITY_MAILBOX_H
#define YUAN_GAME_BASE_ENTITY_MAILBOX_H

#include "game_base/types.h"

#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace yuan::game_base
{
    struct EntityMessage
    {
        EntityId source = 0;
        EntityId target = 0;
        CommandId command = 0;
        Bytes payload;
    };

    class EntityMailbox
    {
    public:
        void post(EntityMessage message)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queues_[message.target].push_back(std::move(message));
        }

        std::optional<EntityMessage> pop(EntityId target)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = queues_.find(target);
            if (it == queues_.end() || it->second.empty()) {
                return std::nullopt;
            }
            auto message = std::move(it->second.front());
            it->second.pop_front();
            if (it->second.empty()) {
                queues_.erase(it);
            }
            return message;
        }

        std::vector<EntityMessage> drain(EntityId target, std::size_t limit = 0)
        {
            std::vector<EntityMessage> out;
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = queues_.find(target);
            if (it == queues_.end()) {
                return out;
            }

            while (!it->second.empty() && (limit == 0 || out.size() < limit)) {
                out.push_back(std::move(it->second.front()));
                it->second.pop_front();
            }
            if (it->second.empty()) {
                queues_.erase(it);
            }
            return out;
        }

    private:
        mutable std::mutex mutex_;
        std::unordered_map<EntityId, std::deque<EntityMessage>> queues_;
    };
}

#endif
