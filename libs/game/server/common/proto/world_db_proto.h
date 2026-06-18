#ifndef YUAN_GAME_SERVER_COMMON_PROTO_WORLD_DB_PROTO_H
#define YUAN_GAME_SERVER_COMMON_PROTO_WORLD_DB_PROTO_H

#include "common/codec/binary_codec.h"
#include "common/proto/base_proto.h"

#include <cstdint>
#include <string>
#include <vector>

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

    struct SSWorldDbPlayerRolesGetRequest
    {
        PlayerUid player_uid = 0;
        PackedGameServiceId world_service_id = 0;

        YUAN_GAME_BINARY_FIELDS(player_uid, world_service_id)
    };

    struct SSWorldDbPlayerRolesSaveRequest
    {
        PlayerUid player_uid = 0;
        std::vector<SSPlayerRoleInfo> roles;
        std::uint64_t data_version = 0;

        YUAN_GAME_BINARY_FIELDS(player_uid, roles, data_version)
    };

    struct SSWorldDbRoleIdList
    {
        std::vector<RoleId> role_ids;

        YUAN_GAME_BINARY_FIELDS(role_ids)
    };

    struct SSWorldDbPlayerRolesResponse
    {
        bool ok = false;
        std::string message;
        PlayerUid player_uid = 0;
        std::vector<SSPlayerRoleInfo> roles;
        bool created_default_role = false;
        bool missing_role_list = false;
        std::uint64_t data_version = 0;

        YUAN_GAME_BINARY_FIELDS(ok, message, player_uid, roles, created_default_role, missing_role_list, data_version)
    };

    struct SSWorldDbOwnershipCompareAndSetRequest
    {
        RoleId role_id = 0;
        PackedGameServiceId source_zone_service_id = 0;
        std::uint64_t expected_gateway_session_id = 0;
        PackedGameServiceId next_zone_service_id = 0;
        std::uint64_t next_gateway_session_id = 0;

        YUAN_GAME_BINARY_FIELDS(role_id, source_zone_service_id, expected_gateway_session_id, next_zone_service_id, next_gateway_session_id)
    };

    struct SSWorldDbOwnershipCompareAndSetResponse
    {
        bool ok = false;
        std::string message;
        bool applied = false;
        bool has_owner = false;
        RoleId role_id = 0;
        PackedGameServiceId zone_service_id = 0;
        std::uint64_t gateway_session_id = 0;
        std::uint64_t data_version = 0;

        YUAN_GAME_BINARY_FIELDS(ok, message, applied, has_owner, role_id, zone_service_id, gateway_session_id, data_version)
    };
}

#endif
