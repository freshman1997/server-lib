#ifndef YUAN_GAME_SERVER_ZONE_ZONE_SERVER_SERVICE_H
#define YUAN_GAME_SERVER_ZONE_ZONE_SERVER_SERVICE_H

#include "application.h"
#include "common/rpc_network.h"
#include "messaging/process_message_manager.h"
#include "zone/zone_service.h"

#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace yuan::redis
{
    class RedisClient;
}

namespace yuan::game::server
{
    class ZoneServerService final : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
    {
    public:
        ZoneServerService(GameServiceId service_id,
                           GameServiceId global_service_id,
                            GameServiceId world_service_id,
                            std::uint16_t tunnel_port,
                            std::uint16_t listen_port,
                            std::string redis_host,
                            std::uint16_t redis_port,
                            std::uint16_t redis_db,
                            std::string redis_username,
                            std::string redis_password,
                            std::uint16_t redis_connect_timeout_ms,
                            std::uint16_t redis_command_timeout_ms,
                            std::uint16_t redis_flush_interval_ms,
                            std::size_t expected_requests = 1);

        void set_runtime_context(const yuan::app::RuntimeContext &context) override;

        bool init() override;

        void start() override;

        void stop() override;

        [[nodiscard]] bool ok() const;

    private:
        std::uint16_t tunnel_port_ = 0;
        std::uint16_t listen_port_ = 0;
        std::size_t expected_requests_ = 1;
        bool ok_ = false;
        yuan::app::RuntimeContext context_;
        GameServiceId service_id_;
        GameServiceId global_service_id_;
        GameServiceId world_service_id_;
        mutable ProcessMessageManager messaging_;
        ZoneService zone_;
        rpc_network::RpcNetworkServer rpc_server_;

        struct PlayerRuntimeData
        {
            PlayerUid player_uid = 0;
            RoleId role_id = 0;
            std::uint32_t level = 1;
            std::uint64_t exp = 0;
        };

        std::string redis_host_;
        std::uint16_t redis_port_ = 6379;
        std::uint16_t redis_db_ = 0;
        std::string redis_username_;
        std::string redis_password_;
        std::uint16_t redis_connect_timeout_ms_ = 1000;
        std::uint16_t redis_command_timeout_ms_ = 1000;
        std::uint16_t redis_flush_interval_ms_ = 5000;
        std::shared_ptr<yuan::redis::RedisClient> redis_;
        std::mutex player_cache_mutex_;
        std::unordered_map<RoleId, PlayerRuntimeData> players_by_role_;
        std::unordered_set<RoleId> dirty_roles_;
        std::jthread flush_thread_;

        bool register_to_tunnel();
        bool register_to_world();
        bool update_world_player_zone(PlayerZoneUpdate update) const;
        bool player_enter(ClientLoginRequest request);
        bool player_leave(ClientLoginRequest request);
        [[nodiscard]] PlayerRuntimeData load_or_create_player(ClientLoginRequest request) const;
        void mark_player_dirty(RoleId role_id);
        void flush_dirty_players();
        void flush_loop(std::stop_token stop_token);
    };
}

#endif
