#ifndef YUAN_GAME_BASE_SESSION_REGISTRY_H
#define YUAN_GAME_BASE_SESSION_REGISTRY_H

#include "game_base/types.h"

#include <mutex>
#include <optional>
#include <unordered_map>

namespace yuan::game_base
{
    struct Session
    {
        SessionId id = 0;
        PlayerId player = 0;
        NodeId gateway = 0;
        std::string remote_address;
        Tags tags;
    };

    class SessionRegistry
    {
    public:
        bool bind(Session session)
        {
            if (session.id == 0) {
                return false;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            if (session.player != 0) {
                player_index_[session.player] = session.id;
            }
            sessions_[session.id] = std::move(session);
            return true;
        }

        bool unbind(SessionId id)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = sessions_.find(id);
            if (it == sessions_.end()) {
                return false;
            }
            if (it->second.player != 0) {
                player_index_.erase(it->second.player);
            }
            sessions_.erase(it);
            return true;
        }

        std::optional<Session> find(SessionId id) const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = sessions_.find(id);
            if (it == sessions_.end()) {
                return std::nullopt;
            }
            return it->second;
        }

        std::optional<Session> find_by_player(PlayerId player) const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto index = player_index_.find(player);
            if (index == player_index_.end()) {
                return std::nullopt;
            }
            const auto it = sessions_.find(index->second);
            if (it == sessions_.end()) {
                return std::nullopt;
            }
            return it->second;
        }

        [[nodiscard]] std::size_t size() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return sessions_.size();
        }

    private:
        mutable std::mutex mutex_;
        std::unordered_map<SessionId, Session> sessions_;
        std::unordered_map<PlayerId, SessionId> player_index_;
    };
}

#endif
