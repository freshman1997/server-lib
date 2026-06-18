#ifndef YUAN_GAME_SERVER_COMMON_PROTO_PLAYER_DB_PROTO_H
#define YUAN_GAME_SERVER_COMMON_PROTO_PLAYER_DB_PROTO_H

#include "common/codec/binary_codec.h"
#include "common/proto/base_proto.h"

#include <cstdint>
#include <string>

namespace yuan::game::server
{
    struct SSPlayerRoleData
    {
        PlayerUid player_uid = 0;
        RoleId role_id = 0;
        std::uint32_t level = 1;
        std::uint64_t exp = 0;

        YUAN_GAME_BINARY_FIELDS(player_uid, role_id, level, exp)
    };

    struct SSPlayerDbLoadRoleRequest
    {
        PlayerUid player_uid = 0;
        RoleId role_id = 0;

        YUAN_GAME_BINARY_FIELDS(player_uid, role_id)
    };

    struct SSPlayerDbSaveRoleRequest
    {
        SSPlayerRoleData role;
        std::uint64_t data_version = 0;

        YUAN_GAME_BINARY_FIELDS(role, data_version)
    };

    struct SSPlayerDbRoleResponse
    {
        bool ok = false;
        std::string message;
        bool has_role = false;
        SSPlayerRoleData role;
        std::uint64_t data_version = 0;

        YUAN_GAME_BINARY_FIELDS(ok, message, has_role, role, data_version)
    };
}

#endif
