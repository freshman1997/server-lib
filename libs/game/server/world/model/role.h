#ifndef YUAN_GAME_SERVER_WORLD_MODEL_ROLE_H
#define YUAN_GAME_SERVER_WORLD_MODEL_ROLE_H

#include "common/game_messages.h"

#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace yuan::redis
{
    class RedisClient;
}

namespace yuan::game::server
{
    class RoleCache
    {
    public:
        void ensure_player_roles(PlayerUid player_uid,
                                 yuan::redis::RedisClient *redis,
                                 PackedGameServiceId world_service_id,
                                 std::function<void(PlayerUid, const PlayerRoleInfo &)> add_role);
        void mark_role_zone(PlayerId player_id, PackedGameServiceId zone_service_id);
        void flush_dirty(yuan::redis::RedisClient *redis, PackedGameServiceId world_service_id);
        [[nodiscard]] std::size_t dirty_player_count() const;
        [[nodiscard]] std::size_t dirty_role_count() const;

    private:
        [[nodiscard]] std::optional<PlayerRoleInfo> load_role(RoleId role_id, yuan::redis::RedisClient *redis) const;
        [[nodiscard]] PlayerRoleInfo create_role(PlayerUid player_uid, yuan::redis::RedisClient *redis, PackedGameServiceId world_service_id) const;
        [[nodiscard]] std::string random_role_name() const;

        mutable std::mutex mutex_;
        std::unordered_set<PlayerUid> loaded_players_;
        std::unordered_map<PlayerUid, std::vector<RoleId>> role_ids_by_player_uid_;
        std::unordered_map<RoleId, PlayerRoleInfo> roles_by_id_;
        std::unordered_set<PlayerUid> dirty_players_;
        std::unordered_set<RoleId> dirty_roles_;
    };
}

#endif
