#ifndef YUAN_GAME_SERVER_ZONE_ZONE_SERVER_SERVICE_H
#define YUAN_GAME_SERVER_ZONE_ZONE_SERVER_SERVICE_H

#include "application.h"
#include "common/rpc_network.h"
#include "messaging/process_message_manager.h"
#include "zone/model/player_manager.h"
#include "zone/rpc/zone_msg_echo.h"
#include "zone/rpc/zone_msg_gm.h"
#include "zone/rpc/zone_msg_player.h"

#include <cstdint>
#include <thread>

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
                              std::string listen_host,
                              std::vector<rpc_network::RpcEndpoint> tunnel_endpoints,
                             std::uint16_t listen_port,
                            std::string redis_host,
                            std::uint16_t redis_port,
                            std::uint16_t redis_db,
                            std::string redis_username,
                            std::string redis_password,
                               std::uint16_t redis_connect_timeout_ms,
                               std::uint16_t redis_command_timeout_ms,
                               std::uint16_t redis_flush_interval_ms,
                               std::uint16_t zone_load_sync_interval_ms,
                               std::uint32_t zone_max_players,
                               std::uint64_t tunnel_heartbeat_interval_ms,
                               std::vector<GatewayInfo> gateway_endpoints);

        void set_runtime_context(const yuan::app::RuntimeContext &context) override;

        bool init() override;

        void start() override;

        void stop() override;

        [[nodiscard]] bool ok() const;

    private:
        std::string listen_host_;
        std::uint16_t listen_port_ = 0;
        bool ok_ = false;
        yuan::app::RuntimeContext context_;
        GameServiceId service_id_;
        GameServiceId global_service_id_;
        GameServiceId world_service_id_;
        mutable ProcessMessageManager messaging_;
        ServiceAddress zone_address_;
        yuan::rpc::Server zone_rpc_;
        rpc_network::RpcNetworkServer rpc_server_;

        std::string redis_host_;
        std::uint16_t redis_port_ = 6379;
        std::uint16_t redis_db_ = 0;
        std::string redis_username_;
        std::string redis_password_;
        std::uint16_t redis_connect_timeout_ms_ = 1000;
        std::uint16_t redis_command_timeout_ms_ = 1000;
        std::uint16_t redis_flush_interval_ms_ = 5000;
        std::uint16_t zone_load_sync_interval_ms_ = 1000;
        std::uint32_t zone_max_players_ = 0;
        std::vector<GatewayInfo> gateway_endpoints_;
        std::shared_ptr<yuan::redis::RedisClient> redis_;
        PlayerManager players_;
        std::jthread flush_thread_;
        std::jthread load_sync_thread_;
        std::jthread register_thread_;

        bool register_to_tunnel();
        void register_loop(std::stop_token stop_token);
        bool register_to_world();
        bool report_zone_load() const;
        bool update_world_player_zone(PlayerZoneUpdate update) const;
        bool player_enter(ClientLoginRequest request);
        bool player_leave(ClientLoginRequest request);
        GmCommandResponse execute_gm(GmCommandRequest request);
        void flush_dirty_players();
        void flush_loop(std::stop_token stop_token);
        void load_sync_loop(std::stop_token stop_token);
    };
}

#endif
