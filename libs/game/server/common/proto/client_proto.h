#ifndef YUAN_GAME_SERVER_COMMON_PROTO_CLIENT_PROTO_H
#define YUAN_GAME_SERVER_COMMON_PROTO_CLIENT_PROTO_H

#include "common/proto/base_proto.h"

#include "yuan/rpc/types.h"

namespace yuan::game::server
{
    struct CSLoginRequest
    {
        PlayerUid player_uid = 0;
        RoleId role_id = 0;
        LoginTokenId login_token_id;
        std::uint64_t gateway_session_id = 0;

        YUAN_GAME_BINARY_FIELDS(player_uid, role_id, login_token_id)
    };

    struct CSLoginResponse
    {
        bool ok = false;
        RoleId role_id = 0;
        PackedGameServiceId zone_service_id = 0;
        std::uint64_t gateway_session_id = 0;
        std::string message;

        YUAN_GAME_BINARY_FIELDS(ok, role_id, message)
    };

    struct CSGameRequest
    {
        PlayerUid player_uid = 0;
        RoleId role_id = 0;
        std::uint64_t gateway_session_id = 0;
        yuan::rpc::Bytes payload;

        YUAN_GAME_BINARY_FIELDS(player_uid, role_id, payload)
    };

    struct CSGameResponse
    {
        bool ok = false;
        RoleId role_id = 0;
        std::uint64_t gateway_session_id = 0;
        yuan::rpc::Bytes payload;
        std::string message;

        YUAN_GAME_BINARY_FIELDS(ok, role_id, payload, message)
    };

    struct CSPushMessage
    {
        RoleId role_id = 0;
        std::uint64_t gateway_session_id = 0;
        yuan::rpc::Bytes payload;
        std::string message;

        YUAN_GAME_BINARY_FIELDS(role_id, gateway_session_id, payload, message)
    };

    struct CSTimeSyncRequest
    {
        PlayerUid player_uid = 0;
        RoleId role_id = 0;
        std::uint64_t gateway_session_id = 0;
        std::uint64_t client_time_seconds = 0;

        YUAN_GAME_BINARY_FIELDS(player_uid, role_id, client_time_seconds)
    };
    struct CSTimeSyncResponse
    {
        bool ok = false;
        RoleId role_id = 0;
        std::uint64_t client_time_seconds = 0;
        std::uint64_t server_receive_time_seconds = 0;
        std::uint64_t server_send_time_seconds = 0;
        std::string message;

        YUAN_GAME_BINARY_FIELDS(ok, role_id, client_time_seconds, server_receive_time_seconds, server_send_time_seconds, message)
    };
}

#endif
