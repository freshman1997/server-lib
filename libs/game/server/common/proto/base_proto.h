#ifndef YUAN_GAME_SERVER_COMMON_PROTO_BASE_PROTO_H
#define YUAN_GAME_SERVER_COMMON_PROTO_BASE_PROTO_H

#include "common/service_node.h"
#include "common/login_token.h"
#include "common/codec/binary_codec.h"

#include <cstdint>
#include <string>
#include <vector>

namespace yuan::game::server
{
    using PlayerUid = std::uint64_t;
    using AccountId = PlayerUid;
    using PlayerId = std::uint64_t;
    using RoleId = PlayerId;

    struct SSGatewayInfo
    {
        PackedGameServiceId service_id = 0;
        std::string host;
        std::uint16_t port = 0;
        std::string name;

        YUAN_GAME_BINARY_FIELDS(service_id, host, port, name)
    };

    struct SSPlayerRoleInfo
    {
        RoleId role_id = 0;
        std::string name;
        std::uint32_t level = 1;
        PackedGameServiceId world_service_id = 0;
        PackedGameServiceId zone_service_id = 0;
        LoginTokenId login_token_id;

        YUAN_GAME_BINARY_FIELDS(role_id, name, level, world_service_id, zone_service_id)
    };

    struct SSZoneInfo
    {
        PackedGameServiceId service_id = 0;
        std::string name;
        std::uint32_t online_players = 0;
        std::uint32_t max_players = 0;
        bool available = true;
        std::vector<SSGatewayInfo> gateways;
        std::string world_routing_strategy = "fixed";
        std::uint64_t world_routing_version = 1;
        std::uint16_t world_count = 1;

        YUAN_GAME_BINARY_FIELDS(service_id, name, online_players, max_players, available, gateways, world_routing_strategy, world_routing_version, world_count)
    };
}

#endif
