#ifndef YUAN_GAME_BASE_TYPES_H
#define YUAN_GAME_BASE_TYPES_H

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace yuan::game_base
{
    using NodeId = std::uint64_t;
    using ShardId = std::uint32_t;
    using ZoneId = std::uint32_t;
    using SceneId = std::uint64_t;
    using RoomId = std::uint64_t;
    using EntityId = std::uint64_t;
    using PlayerId = std::uint64_t;
    using SessionId = std::uint64_t;
    using CommandId = std::uint32_t;
    using Milliseconds = std::chrono::milliseconds;
    using Bytes = std::vector<std::uint8_t>;
    using Tags = std::unordered_map<std::string, std::string>;

    enum class GameGenre : std::uint8_t
    {
        generic,
        slg,
        rpg,
        act,
        moba,
        mmorpg
    };

    enum class ServerRole : std::uint16_t
    {
        gateway,
        login,
        lobby,
        match,
        room,
        scene,
        world,
        battle,
        chat,
        social,
        database,
        gm,
        metrics
    };

    enum class ServiceState : std::uint8_t
    {
        created,
        starting,
        running,
        stopping,
        stopped,
        failed
    };

    enum class DeliveryGuarantee : std::uint8_t
    {
        best_effort,
        at_least_once,
        ordered
    };

    struct NodeKey
    {
        NodeId id = 0;
        ServerRole role = ServerRole::gateway;
        ShardId shard = 0;
        ZoneId zone = 0;
    };

    struct Endpoint
    {
        std::string host;
        std::uint16_t port = 0;
        std::string protocol = "tcp";
    };

    struct NodeDescriptor
    {
        NodeKey key;
        Endpoint public_endpoint;
        Endpoint internal_endpoint;
        Tags tags;
    };

    inline std::string_view to_string(ServerRole role)
    {
        switch (role) {
            case ServerRole::gateway:
                return "gateway";
            case ServerRole::login:
                return "login";
            case ServerRole::lobby:
                return "lobby";
            case ServerRole::match:
                return "match";
            case ServerRole::room:
                return "room";
            case ServerRole::scene:
                return "scene";
            case ServerRole::world:
                return "world";
            case ServerRole::battle:
                return "battle";
            case ServerRole::chat:
                return "chat";
            case ServerRole::social:
                return "social";
            case ServerRole::database:
                return "database";
            case ServerRole::gm:
                return "gm";
            case ServerRole::metrics:
                return "metrics";
        }
        return "unknown";
    }
}

#endif
