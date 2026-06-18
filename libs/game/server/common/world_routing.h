#ifndef YUAN_GAME_SERVER_COMMON_WORLD_ROUTING_H
#define YUAN_GAME_SERVER_COMMON_WORLD_ROUTING_H

#include "common/proto/base_proto.h"
#include "common/service_id.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace yuan::game::server
{
    struct WorldRoutingConfig
    {
        std::string strategy = "fixed";
        std::uint64_t version = 1;
        std::uint16_t world_count = 1;
    };

    struct WorldEndpointConfig
    {
        std::uint16_t world = 0;
        std::string host;
        std::uint16_t port = 0;
        std::string state = "open";
    };

    inline std::optional<std::uint16_t> route_world_number_by_player_uid(PlayerUid player_uid, const WorldRoutingConfig &config)
    {
        if (player_uid == 0 || config.world_count == 0) {
            return std::nullopt;
        }
        if (config.strategy == "modulo") {
            return static_cast<std::uint16_t>(1 + static_cast<std::uint16_t>(player_uid % config.world_count));
        }
        return 1;
    }

    inline std::optional<GameServiceId> route_world_service_by_player_uid(PlayerUid player_uid,
                                                                         const WorldRoutingConfig &config,
                                                                         std::uint16_t region,
                                                                         std::uint64_t instance)
    {
        const auto world = route_world_number_by_player_uid(player_uid, config);
        if (!world) {
            return std::nullopt;
        }
        GameServiceId service;
        service.region = region;
        service.world = *world;
        service.type = GameServiceType::world;
        service.shard = *world;
        service.instance = instance == 0 ? 1 : instance;
        return service;
    }
}

#endif
