#ifndef YUAN_GAME_SERVER_COMMON_PROTO_GATEWAY_PROTO_H
#define YUAN_GAME_SERVER_COMMON_PROTO_GATEWAY_PROTO_H

#include "common/proto/base_proto.h"

namespace yuan::game::server
{
    struct SSGatewayLoginRequest
    {
        PlayerUid player_uid = 0;
        RoleId role_id = 0;
        PackedGameServiceId zone_service_id = 0;
        std::uint64_t gateway_session_id = 0;

        YUAN_GAME_BINARY_FIELDS(player_uid, role_id, zone_service_id, gateway_session_id)
    };

    struct SSGatewayLoginResponse
    {
        bool ok = false;
        RoleId role_id = 0;
        PackedGameServiceId zone_service_id = 0;
        std::uint64_t gateway_session_id = 0;
        std::string message;

        YUAN_GAME_BINARY_FIELDS(ok, role_id, zone_service_id, gateway_session_id, message)
    };
}

#endif
