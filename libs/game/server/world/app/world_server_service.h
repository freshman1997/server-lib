#ifndef YUAN_GAME_SERVER_WORLD_WORLD_SERVER_SERVICE_H
#define YUAN_GAME_SERVER_WORLD_WORLD_SERVER_SERVICE_H

#include "application.h"
#include "common/db_proxy_routing.h"
#include "common/rpc_network.h"
#include "http_server.h"
#include "messaging/process_message_manager.h"
#include "world/model/role.h"
#include "world/rpc/world_msg.h"

#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace yuan::redis
{
    class RedisClient;
}

namespace yuan::game::server
{
    class WorldServerService final : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
    {
    public:
        WorldServerService(GameServiceId service_id,
                           std::string listen_host,
                           std::uint16_t port,
                           std::uint16_t http_port,
                            std::vector<rpc_network::RpcEndpoint> tunnel_endpoints,
                           std::string redis_host,
                           std::uint16_t redis_port,
                           std::uint16_t redis_db,
                           std::string redis_username,
                           std::string redis_password,
                               std::uint16_t redis_connect_timeout_ms,
                               std::uint16_t redis_command_timeout_ms,
                               std::uint16_t redis_flush_interval_ms,
                               std::string world_ownership_store,
                                std::uint64_t login_reservation_ttl_ms,
                                std::uint64_t zone_report_ttl_ms,
                                 std::uint64_t tunnel_heartbeat_interval_ms,
                                  std::uint64_t login_token_secret,
                                  WorldRoutingConfig world_routing,
                                  DbProxyRoutingConfig world_db_proxy_routing,
                                  std::uint64_t metrics_log_interval_ms = 0);

        void set_runtime_context(const yuan::app::RuntimeContext &context) override;
        bool init() override;
        void start() override;
        void stop() override;

        [[nodiscard]] bool ok() const;

    private:
        bool register_to_tunnel();
        void register_loop(std::stop_token stop_token);
        void register_http_routes();
        void ensure_player_roles(PlayerUid player_uid);
        void mark_role_dirty(PlayerId player_id, PackedGameServiceId zone_service_id);
        bool save_role_location_to_db(PlayerId player_id, PackedGameServiceId zone_service_id) const;
        [[nodiscard]] PlayerUid player_uid_for_role(PlayerId player_id) const;
        [[nodiscard]] std::optional<SSGmCommandResponse> forward_gm(SSGmCommandRequest request) const;
        void flush_dirty_roles();
        void flush_loop(std::stop_token stop_token);
        void metrics_loop(std::stop_token stop_token) const;

        std::string listen_host_;
        std::uint16_t port_ = 0;
        std::uint16_t http_port_ = 0;
        bool ok_ = false;
        yuan::app::RuntimeContext context_;
        GameServiceId service_id_;
        std::string redis_host_;
        std::uint16_t redis_port_ = 6379;
        std::uint16_t redis_db_ = 0;
        std::string redis_username_;
        std::string redis_password_;
        std::uint16_t redis_connect_timeout_ms_ = 1000;
        std::uint16_t redis_command_timeout_ms_ = 1000;
        std::uint16_t redis_flush_interval_ms_ = 5000;
        std::string world_ownership_store_ = "memory";
        std::atomic<bool> draining_{false};
        std::uint64_t metrics_log_interval_ms_ = 0;
        DbProxyRoutingConfig world_db_proxy_routing_;
        mutable std::mutex pending_role_locations_mutex_;
        std::unordered_map<PlayerId, PackedGameServiceId> pending_role_locations_;
        WorldMsgContext world_context_;
        yuan::rpc::Server world_rpc_;
        RoleCache roles_;
        std::shared_ptr<yuan::redis::RedisClient> redis_;
        std::jthread flush_thread_;
        std::jthread metrics_thread_;
        std::jthread register_thread_;
        mutable ProcessMessageManager messaging_;
        rpc_network::RpcNetworkServer rpc_server_;
        std::unique_ptr<yuan::net::http::HttpServer> http_server_;
        std::jthread http_thread_;
    };
}

#endif
