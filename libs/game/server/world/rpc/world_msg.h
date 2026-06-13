#ifndef YUAN_GAME_SERVER_WORLD_RPC_WORLD_MSG_H
#define YUAN_GAME_SERVER_WORLD_RPC_WORLD_MSG_H

#include "common/game_messages.h"
#include "world/model/world_ownership_store.h"

#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace yuan::game::server
{
    struct ZoneLoginReservation
    {
        PackedGameServiceId zone_service_id = 0;
        std::uint64_t expires_at_ms = 0;
    };

    struct WorldMsgContext
    {
        ServiceAddress address;
        std::unordered_map<PlayerUid, std::vector<PlayerRoleInfo>> roles_by_player_uid;
        std::unordered_map<PlayerId, PackedGameServiceId> zone_by_player;
        std::unordered_map<PlayerId, std::uint64_t> session_by_player;
        std::shared_ptr<WorldOwnershipStore> ownership_store;
        std::unordered_map<PlayerId, ZoneLoginReservation> pending_login_by_role;
        std::uint64_t login_reservation_ttl_ms = 3000;
        std::uint64_t zone_report_ttl_ms = 3000;
        std::vector<GatewayInfo> gateways;
        std::unordered_map<PackedGameServiceId, ZoneInfo> zones;
        std::unordered_map<PackedGameServiceId, std::uint64_t> zone_last_report_ms;
        std::function<void(PlayerUid)> before_login_options;
        std::function<void(PlayerId, PackedGameServiceId)> after_player_zone_set;
        std::function<std::optional<GmCommandResponse>(GmCommandRequest)> gm_forward_handler;
    };

    void world_add_role(WorldMsgContext &context, PlayerUid player_uid, PlayerRoleInfo role);
    void world_register_gateway(WorldMsgContext &context, GatewayInfo gateway);
    void world_register_zone(WorldMsgContext &context, ZoneInfo zone);
    void world_mark_stale_zones_unavailable(WorldMsgContext &context, std::uint64_t now_ms);
    bool world_set_player_zone(WorldMsgContext &context, PlayerId player_id, PackedGameServiceId zone_service_id);
    bool world_set_player_zone(WorldMsgContext &context, PlayerId player_id, PackedGameServiceId zone_service_id, PackedGameServiceId source_zone_service_id);
    bool world_set_player_zone(WorldMsgContext &context, PlayerId player_id, PackedGameServiceId zone_service_id, PackedGameServiceId source_zone_service_id, std::uint64_t gateway_session_id);
    void world_prune_expired_login_reservations(WorldMsgContext &context, std::uint64_t now_ms);
    [[nodiscard]] std::optional<PackedGameServiceId> world_player_zone(const WorldMsgContext &context, PlayerId player_id);
    [[nodiscard]] std::optional<PackedGameServiceId> world_select_zone(const WorldMsgContext &context, PlayerUid player_uid, RoleId role_id);
    [[nodiscard]] LoginOptionsResponse world_login_options(WorldMsgContext &context, PlayerUid player_uid);

    bool register_world_msg(yuan::rpc::Server &server, WorldMsgContext &context);
}

#endif
