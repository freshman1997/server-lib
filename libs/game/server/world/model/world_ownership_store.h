#ifndef YUAN_GAME_SERVER_WORLD_MODEL_WORLD_OWNERSHIP_STORE_H
#define YUAN_GAME_SERVER_WORLD_MODEL_WORLD_OWNERSHIP_STORE_H

#include "common/codec/game_binary_codec.h"

#include "redis_client.h"

#include <memory>
#include <optional>
#include <string>
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

    class RedisWorldOwnershipStore final : public WorldOwnershipStore
    {
    public:
        RedisWorldOwnershipStore(std::shared_ptr<yuan::redis::RedisClient> redis, std::string key_prefix = "game:world:owner:");

        [[nodiscard]] std::optional<WorldOwnershipRecord> get(PlayerId player_id) const override;
        [[nodiscard]] bool compare_and_set(PlayerId player_id,
                                           PackedGameServiceId source_zone_service_id,
                                           std::uint64_t expected_gateway_session_id,
                                           WorldOwnershipRecord next) override;

    private:
        [[nodiscard]] std::string key(PlayerId player_id) const;

        std::shared_ptr<yuan::redis::RedisClient> redis_;
        std::string key_prefix_;
    };
}

#endif
