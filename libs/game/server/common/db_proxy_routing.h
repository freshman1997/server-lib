#ifndef YUAN_GAME_SERVER_COMMON_DB_PROXY_ROUTING_H
#define YUAN_GAME_SERVER_COMMON_DB_PROXY_ROUTING_H

#include "common/service_id.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace yuan::game::server
{
    struct DbProxyEndpointConfig
    {
        PackedGameServiceId service_id = 0;
        std::uint32_t shard = 0;
        std::string state = "open";
    };

    struct DbProxyRoutingConfig
    {
        std::string strategy = "modulo";
        std::uint64_t version = 1;
        std::uint32_t shard_count = 1;
        std::vector<DbProxyEndpointConfig> endpoints;
    };

    inline std::optional<PackedGameServiceId> select_db_proxy(std::uint64_t owner_id, const DbProxyRoutingConfig &routing)
    {
        if (owner_id == 0 || routing.endpoints.empty() || routing.shard_count == 0 || routing.strategy != "modulo") {
            return std::nullopt;
        }
        const auto shard = static_cast<std::uint32_t>(owner_id % routing.shard_count);
        for (const auto &endpoint : routing.endpoints) {
            if (endpoint.shard == shard && endpoint.service_id != 0 && endpoint.state == "open") {
                return endpoint.service_id;
            }
        }
        return std::nullopt;
    }
}

#endif
