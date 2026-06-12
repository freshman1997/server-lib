#ifndef YUAN_GAME_SERVER_COMMON_SERVICE_ID_H
#define YUAN_GAME_SERVER_COMMON_SERVICE_ID_H

#include "game_base/types.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace yuan::game::server
{
    using ServiceTypeId = std::uint16_t;
    using ServiceRegionId = std::uint16_t;
    using ServiceWorldId = std::uint16_t;
    using ServiceInstanceId = std::uint64_t;
    using PackedGameServiceId = std::uint64_t;

    inline constexpr std::uint8_t service_instance_bits = 28;
    inline constexpr std::uint8_t service_type_bits = 12;
    inline constexpr std::uint8_t service_world_bits = 12;
    inline constexpr std::uint8_t service_region_bits = 12;
    inline constexpr PackedGameServiceId service_instance_mask = (PackedGameServiceId{1} << service_instance_bits) - 1;
    inline constexpr PackedGameServiceId service_type_mask = (PackedGameServiceId{1} << service_type_bits) - 1;
    inline constexpr PackedGameServiceId service_world_mask = (PackedGameServiceId{1} << service_world_bits) - 1;
    inline constexpr PackedGameServiceId service_region_mask = (PackedGameServiceId{1} << service_region_bits) - 1;

    enum class GameServiceType : ServiceTypeId
    {
        tunnel = 1,
        zone = 2,
        global = 3,
        gateway = 4,
        login = 5,
        match = 6,
        battle = 7,
        chat = 8,
        world = 9,
        web = 10
    };

    struct GameServiceId
    {
        ServiceRegionId region = 0;
        ServiceWorldId world = 0;
        GameServiceType type = GameServiceType::tunnel;
        yuan::game_base::ShardId shard = 0;
        ServiceInstanceId instance = 0;

        bool operator==(const GameServiceId &other) const
        {
            return region == other.region && world == other.world && type == other.type && shard == other.shard && instance == other.instance;
        }

        [[nodiscard]] PackedGameServiceId pack() const
        {
            const auto packed_region = static_cast<PackedGameServiceId>(region) & service_region_mask;
            const auto packed_world = static_cast<PackedGameServiceId>(world != 0 ? world : shard) & service_world_mask;
            const auto packed_type = static_cast<PackedGameServiceId>(static_cast<ServiceTypeId>(type)) & service_type_mask;
            const auto packed_instance = static_cast<PackedGameServiceId>(instance) & service_instance_mask;
            return (packed_region << (service_world_bits + service_type_bits + service_instance_bits)) |
                   (packed_world << (service_type_bits + service_instance_bits)) |
                   (packed_type << service_instance_bits) |
                   packed_instance;
        }
    };

    inline GameServiceId unpack_game_service_id(PackedGameServiceId packed)
    {
        GameServiceId id;
        id.instance = packed & service_instance_mask;
        id.type = static_cast<GameServiceType>((packed >> service_instance_bits) & service_type_mask);
        id.world = static_cast<ServiceWorldId>((packed >> (service_instance_bits + service_type_bits)) & service_world_mask);
        id.shard = id.world;
        id.region = static_cast<ServiceRegionId>((packed >> (service_instance_bits + service_type_bits + service_world_bits)) & service_region_mask);
        return id;
    }

    inline PackedGameServiceId pack_game_service_id(ServiceRegionId region,
                                                    ServiceWorldId world,
                                                    GameServiceType type,
                                                    ServiceInstanceId instance)
    {
        return GameServiceId{region, world, type, world, instance}.pack();
    }

    inline std::string_view to_string(GameServiceType type)
    {
        switch (type) {
            case GameServiceType::tunnel:
                return "tunnel";
            case GameServiceType::zone:
                return "zone";
            case GameServiceType::global:
                return "global";
            case GameServiceType::gateway:
                return "gateway";
            case GameServiceType::login:
                return "login";
            case GameServiceType::match:
                return "match";
            case GameServiceType::battle:
                return "battle";
            case GameServiceType::chat:
                return "chat";
            case GameServiceType::world:
                return "world";
            case GameServiceType::web:
                return "web";
        }
        return "unknown";
    }

    inline std::string service_id_key(const GameServiceId &id)
    {
        return std::to_string(id.pack());
    }

    inline std::string service_id_debug_string(const GameServiceId &id)
    {
        const auto world = id.world != 0 ? id.world : static_cast<ServiceWorldId>(id.shard);
        return "region:" + std::to_string(id.region) + ":world:" + std::to_string(world) + ":type:" +
               std::string(to_string(id.type)) + ":instance:" + std::to_string(id.instance) + ":packed:" + std::to_string(id.pack());
    }

    namespace route
    {
        inline constexpr std::string_view tunnel_forward = "tunnel.forward";
        inline constexpr std::string_view tunnel_reply = "tunnel.reply";
        inline constexpr std::string_view tunnel_register = "tunnel.register";
        inline constexpr std::string_view zone_echo = "zone.echo";
        inline constexpr std::string_view global_echo = "global.echo";
        inline constexpr std::string_view global_player_lookup = "global.player.lookup";
        inline constexpr std::string_view gateway_login_options = "gateway.login.options";
        inline constexpr std::string_view gateway_login = "gateway.login";
        inline constexpr std::string_view web_bootstrap = "web.bootstrap";
        inline constexpr std::string_view world_login_options = "world.login.options";
        inline constexpr std::string_view world_gateway_register = "world.gateway.register";
        inline constexpr std::string_view world_zone_register = "world.zone.register";
        inline constexpr std::string_view world_zone_select = "world.zone.select";
        inline constexpr std::string_view world_player_zone_get = "world.player.zone.get";
        inline constexpr std::string_view world_player_zone_set = "world.player.zone.set";
        inline constexpr std::string_view zone_player_enter = "zone.player.enter";
        inline constexpr std::string_view zone_player_leave = "zone.player.leave";
    }
}

#endif
