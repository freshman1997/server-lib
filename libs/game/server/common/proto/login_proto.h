#ifndef YUAN_GAME_SERVER_COMMON_PROTO_LOGIN_PROTO_H
#define YUAN_GAME_SERVER_COMMON_PROTO_LOGIN_PROTO_H

#include "common/proto/base_proto.h"

namespace yuan::game::server
{
    struct LoginOptionsRequest
    {
        PlayerUid player_uid = 0;

        YUAN_GAME_BINARY_FIELDS(player_uid)
    };

    struct LoginOptionsResponse
    {
        std::vector<SSGatewayInfo> gateways;
        std::vector<SSPlayerRoleInfo> roles;
        std::vector<SSZoneInfo> zones;

        YUAN_GAME_BINARY_FIELDS(gateways, roles)
    };

    struct SSPlayerZoneQuery
    {
        PlayerId player_id = 0;

        YUAN_GAME_BINARY_FIELDS(player_id)
    };

    struct SSZoneSelectRequest
    {
        PlayerUid player_uid = 0;
        RoleId role_id = 0;

        YUAN_GAME_BINARY_FIELDS(player_uid, role_id)
    };

    struct SSPlayerZoneUpdate
    {
        PlayerUid player_uid = 0;
        PlayerId player_id = 0;
        PackedGameServiceId zone_service_id = 0;
        PackedGameServiceId source_zone_service_id = 0;
        std::uint64_t gateway_session_id = 0;

        YUAN_GAME_BINARY_FIELDS(player_uid, player_id, zone_service_id, source_zone_service_id, gateway_session_id)
    };
}

#endif
