#ifndef YUAN_GAME_SERVER_ZONE_MODEL_PLAYER_MANAGER_H
#define YUAN_GAME_SERVER_ZONE_MODEL_PLAYER_MANAGER_H

#include "zone/model/player.h"

#include <functional>
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
        using PlayerLoader = std::function<std::optional<Player>(SSGatewayLoginRequest)>;
        using PlayerSaver = std::function<bool(const Player &)>;

        bool online(SSGatewayLoginRequest request, const PlayerLoader &loader = {});
        bool offline(SSGatewayLoginRequest request);
        bool set_level(RoleId role_id, std::uint32_t level);
        void flush_dirty(const PlayerSaver &saver, PackedGameServiceId zone_service_id);
        [[nodiscard]] std::size_t dirty_count() const;
        [[nodiscard]] std::size_t online_count() const;
        [[nodiscard]] bool role_online(RoleId role_id) const;
        [[nodiscard]] std::uint64_t gateway_session_for_role(RoleId role_id) const;
        [[nodiscard]] RoleId role_for_gateway_session(std::uint64_t gateway_session_id) const;
        [[nodiscard]] PlayerUid player_uid_for_role(RoleId role_id) const;

    private:
        [[nodiscard]] Player load_or_create(SSGatewayLoginRequest request, const PlayerLoader &loader) const;
        void mark_dirty(RoleId role_id);

        mutable std::mutex mutex_;
        std::unordered_map<RoleId, Player> players_by_role_;
        std::unordered_map<RoleId, std::uint64_t> gateway_session_by_role_;
        std::unordered_map<std::uint64_t, RoleId> role_by_gateway_session_;
        std::unordered_set<RoleId> online_roles_;
        std::unordered_set<RoleId> dirty_roles_;
    };
}

#endif
