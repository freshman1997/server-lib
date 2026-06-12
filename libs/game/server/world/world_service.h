#ifndef YUAN_GAME_SERVER_WORLD_WORLD_SERVICE_H
#define YUAN_GAME_SERVER_WORLD_WORLD_SERVICE_H

#include "common/game_messages.h"

#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

namespace yuan::game::server
{
    class WorldService : public ServiceNode
    {
    public:
        explicit WorldService(ServiceAddress address);

        void add_role(PlayerUid player_uid, PlayerRoleInfo role);
        void register_gateway(GatewayInfo gateway);
        void register_zone(ZoneInfo zone);
        bool set_player_zone(PlayerId player_id, PackedGameServiceId zone_service_id);
        void set_before_login_options(std::function<void(PlayerUid)> handler);
        void set_after_player_zone_set(std::function<void(PlayerId, PackedGameServiceId)> handler);
        void set_gm_forward_handler(std::function<std::optional<GmCommandResponse>(GmCommandRequest)> handler);

        [[nodiscard]] std::optional<PackedGameServiceId> player_zone(PlayerId player_id) const;
        [[nodiscard]] std::optional<PackedGameServiceId> select_zone(PlayerUid player_uid, RoleId role_id) const;
        [[nodiscard]] LoginOptionsResponse login_options(PlayerUid player_uid) const;

    private:
        std::unordered_map<PlayerUid, std::vector<PlayerRoleInfo>> roles_by_player_uid_;
        std::unordered_map<PlayerId, PackedGameServiceId> zone_by_player_;
        std::vector<GatewayInfo> gateways_;
        std::unordered_map<PackedGameServiceId, ZoneInfo> zones_;
        std::function<void(PlayerUid)> before_login_options_;
        std::function<void(PlayerId, PackedGameServiceId)> after_player_zone_set_;
        std::function<std::optional<GmCommandResponse>(GmCommandRequest)> gm_forward_handler_;
    };
}

#endif
