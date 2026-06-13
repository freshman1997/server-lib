#ifndef YUAN_GAME_SERVER_ZONE_MODEL_PLAYER_MANAGER_H
#define YUAN_GAME_SERVER_ZONE_MODEL_PLAYER_MANAGER_H

#include "zone/model/player.h"

#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace yuan::redis
{
    class RedisClient;
}

namespace yuan::game::server
{
    class PlayerManager
    {
    public:
        bool online(ClientLoginRequest request, yuan::redis::RedisClient *redis);
        bool offline(ClientLoginRequest request);
        bool set_level(RoleId role_id, std::uint32_t level);
        void flush_dirty(yuan::redis::RedisClient *redis, PackedGameServiceId zone_service_id);
        [[nodiscard]] std::size_t dirty_count() const;
        [[nodiscard]] std::size_t online_count() const;
        [[nodiscard]] bool role_online(RoleId role_id) const;

    private:
        [[nodiscard]] Player load_or_create(ClientLoginRequest request, yuan::redis::RedisClient *redis) const;
        void mark_dirty(RoleId role_id);

        mutable std::mutex mutex_;
        std::unordered_map<RoleId, Player> players_by_role_;
        std::unordered_set<RoleId> online_roles_;
        std::unordered_set<RoleId> dirty_roles_;
    };
}

#endif
