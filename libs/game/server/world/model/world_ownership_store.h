#ifndef YUAN_GAME_SERVER_WORLD_MODEL_WORLD_OWNERSHIP_STORE_H
#define YUAN_GAME_SERVER_WORLD_MODEL_WORLD_OWNERSHIP_STORE_H

#include "common/codec/game_binary_codec.h"
#include "common/db_proxy_routing.h"
#include "messaging/tunnel_client_manager.h"

#include <optional>
#include <unordered_map>

namespace yuan::game::server
{
    struct WorldOwnershipRecord
    {
        PackedGameServiceId zone_service_id = 0;
        std::uint64_t gateway_session_id = 0;
    };

    class WorldOwnershipStore
    {
    public:
        virtual ~WorldOwnershipStore() = default;
        [[nodiscard]] virtual std::optional<WorldOwnershipRecord> get(PlayerId player_id) const = 0;
        [[nodiscard]] virtual bool compare_and_set(PlayerId player_id,
                                                   PackedGameServiceId source_zone_service_id,
                                                   std::uint64_t expected_gateway_session_id,
                                                   WorldOwnershipRecord next) = 0;
    };

    class InMemoryWorldOwnershipStore final : public WorldOwnershipStore
    {
    public:
        [[nodiscard]] std::optional<WorldOwnershipRecord> get(PlayerId player_id) const override;
        [[nodiscard]] bool compare_and_set(PlayerId player_id,
                                           PackedGameServiceId source_zone_service_id,
                                           std::uint64_t expected_gateway_session_id,
                                           WorldOwnershipRecord next) override;

    private:
        std::unordered_map<PlayerId, WorldOwnershipRecord> records_;
    };

    class WorldDbProxyOwnershipStore final : public WorldOwnershipStore
    {
    public:
        WorldDbProxyOwnershipStore(PackedGameServiceId source_service_id,
                                   DbProxyRoutingConfig routing,
                                   TunnelClientManager &tunnel_client_manager);

        [[nodiscard]] std::optional<WorldOwnershipRecord> get(PlayerId player_id) const override;
        [[nodiscard]] bool compare_and_set(PlayerId player_id,
                                           PackedGameServiceId source_zone_service_id,
                                           std::uint64_t expected_gateway_session_id,
                                           WorldOwnershipRecord next) override;

    private:
        PackedGameServiceId source_service_id_ = 0;
        DbProxyRoutingConfig routing_;
        TunnelClientManager *tunnel_client_manager_ = nullptr;
    };
}

#endif
