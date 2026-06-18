#ifndef YUAN_GAME_SERVER_COMMON_PROTO_WORLD_DB_PROTO_H
#define YUAN_GAME_SERVER_COMMON_PROTO_WORLD_DB_PROTO_H

#include "common/codec/binary_codec.h"
#include "common/proto/base_proto.h"

#include <cstdint>
#include <string>

namespace yuan::game::server
{
    struct SSWorldDbRoleLocationGetRequest
    {
        RoleId role_id = 0;

        YUAN_GAME_BINARY_FIELDS(role_id)
    };

    struct SSWorldDbRoleLocationSetRequest
    {
        PlayerUid player_uid = 0;
        RoleId role_id = 0;
        PackedGameServiceId zone_service_id = 0;
        std::uint64_t gateway_session_id = 0;
        std::uint64_t data_version = 0;

        YUAN_GAME_BINARY_FIELDS(player_uid, role_id, zone_service_id, gateway_session_id, data_version)
    };

    struct SSWorldDbRoleLocationResponse
    {
        bool ok = false;
        std::string message;
        bool has_location = false;
        PlayerUid player_uid = 0;
        RoleId role_id = 0;
        PackedGameServiceId zone_service_id = 0;
        std::uint64_t gateway_session_id = 0;
        std::uint64_t data_version = 0;

        YUAN_GAME_BINARY_FIELDS(ok, message, has_location, player_uid, role_id, zone_service_id, gateway_session_id, data_version)
    };
}

#endif
