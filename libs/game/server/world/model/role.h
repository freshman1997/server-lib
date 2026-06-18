#ifndef YUAN_GAME_SERVER_WORLD_MODEL_ROLE_H
#define YUAN_GAME_SERVER_WORLD_MODEL_ROLE_H

#include "common/codec/game_binary_codec.h"

#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace yuan::game::server
{
    class RoleCache
    {
    public:
        struct LoadedPlayerRoles
        {
            PlayerUid player_uid = 0;
            std::vector<SSPlayerRoleInfo> roles;
            bool created_default_role = false;
            bool missing_role_list = false;
        };
        using PlayerRolesSaver = std::function<bool(PlayerUid, const std::vector<SSPlayerRoleInfo> &)>;

        [[nodiscard]] bool is_player_loaded(PlayerUid player_uid) const;
        void apply_player_roles(const LoadedPlayerRoles &loaded,
                                 std::function<void(PlayerUid, const SSPlayerRoleInfo &)> add_role);
        void mark_role_zone(PlayerId player_id, PackedGameServiceId zone_service_id);
        std::size_t flush_dirty(const PlayerRolesSaver &saver, PackedGameServiceId world_service_id);
        [[nodiscard]] std::size_t dirty_player_count() const;
        [[nodiscard]] std::size_t dirty_role_count() const;

    private:
        mutable std::mutex mutex_;
        std::unordered_set<PlayerUid> loaded_players_;
        std::unordered_map<PlayerUid, std::vector<RoleId>> role_ids_by_player_uid_;
        std::unordered_map<RoleId, SSPlayerRoleInfo> roles_by_id_;
        std::unordered_set<PlayerUid> dirty_players_;
        std::unordered_set<RoleId> dirty_roles_;
    };
}

#endif
