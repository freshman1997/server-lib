#ifndef YUAN_GAME_BASE_ROOM_H
#define YUAN_GAME_BASE_ROOM_H

#include "game_base/types.h"

#include <algorithm>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace yuan::game_base
{
    enum class RoomState : std::uint8_t
    {
        created,
        matching,
        loading,
        running,
        finished,
        closed
    };

    struct Room
    {
        RoomId id = 0;
        std::string mode;
        RoomState state = RoomState::created;
        std::vector<PlayerId> players;
        Tags tags;
    };

    class RoomRegistry
    {
    public:
        bool create(Room room)
        {
            if (room.id == 0) {
                return false;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            return rooms_.emplace(room.id, std::move(room)).second;
        }

        bool join(RoomId room_id, PlayerId player)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = rooms_.find(room_id);
            if (it == rooms_.end() || player == 0) {
                return false;
            }
            auto &players = it->second.players;
            if (std::find(players.begin(), players.end(), player) == players.end()) {
                players.push_back(player);
            }
            return true;
        }

        bool leave(RoomId room_id, PlayerId player)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = rooms_.find(room_id);
            if (it == rooms_.end()) {
                return false;
            }
            auto &players = it->second.players;
            const auto old_size = players.size();
            players.erase(std::remove(players.begin(), players.end(), player), players.end());
            return players.size() != old_size;
        }

        bool set_state(RoomId room_id, RoomState state)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = rooms_.find(room_id);
            if (it == rooms_.end()) {
                return false;
            }
            it->second.state = state;
            return true;
        }

        std::optional<Room> find(RoomId room_id) const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = rooms_.find(room_id);
            if (it == rooms_.end()) {
                return std::nullopt;
            }
            return it->second;
        }

    private:
        mutable std::mutex mutex_;
        std::unordered_map<RoomId, Room> rooms_;
    };
}

#endif
