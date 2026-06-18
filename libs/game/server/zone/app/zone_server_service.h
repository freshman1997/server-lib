#ifndef YUAN_GAME_SERVER_ZONE_ZONE_SERVER_SERVICE_H
#define YUAN_GAME_SERVER_ZONE_ZONE_SERVER_SERVICE_H

#include "application.h"
#include "common/db_proxy_routing.h"
#include "common/rpc_network.h"
#include "common/service_config.h"
#include "common/world_routing.h"
#include "messaging/tunnel_client_manager.h"
#include "timer/timer_handle.h"
#include "zone/model/player_manager.h"
#include "zone/rpc/zone_msg_echo.h"
#include "zone/rpc/zone_msg_gm.h"
#include "zone/rpc/zone_msg_player.h"

#include <cstdint>
#include <optional>
#include <thread>

namespace yuan::game::server
{
    class ZoneServerService final : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
    {
    public:
        explicit ZoneServerService(ServiceServerConfig config);

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
        GameServiceId world_service_id_;
        GameServiceId player_db_proxy_service_id_;
        DbProxyRoutingConfig player_db_proxy_routing_;
        mutable TunnelClientManager tunnel_client_manager_;
        ServiceAddress zone_address_;
        yuan::rpc::Server zone_rpc_;
        rpc_network::RpcNetworkServer rpc_server_;

        std::uint16_t redis_flush_interval_ms_ = 5000;
        std::uint16_t zone_load_sync_interval_ms_ = 1000;
        std::uint32_t zone_max_players_ = 0;
        WorldRoutingConfig world_routing_;
        std::vector<SSGatewayInfo> gateway_endpoints_;
        PlayerManager players_;
        std::jthread flush_thread_;
        yuan::timer::TimerHandle load_sync_timer_;
        bool register_to_world();
        bool report_zone_load() const;
        bool update_world_player_zone(SSPlayerZoneUpdate update) const;
        std::optional<Player> load_player_from_db(SSGatewayLoginRequest request) const;
        bool save_player_to_db(const Player &player) const;
        bool player_enter(SSGatewayLoginRequest request);
        bool player_leave(SSGatewayLoginRequest request);
        SSGmCommandResponse execute_gm(SSGmCommandRequest request);
        std::size_t flush_dirty_players();
        void flush_loop(std::stop_token stop_token);
    };
}

#endif
