#ifndef YUAN_GAME_SERVER_WORLD_WORLD_SERVER_SERVICE_H
#define YUAN_GAME_SERVER_WORLD_WORLD_SERVER_SERVICE_H

#include "application.h"
#include "common/db_proxy_routing.h"
#include "common/rpc_network.h"
#include "common/service_config.h"
#include "coroutine/task.h"
#include "http_server.h"
#include "messaging/tunnel_client_manager.h"
#include "timer/timer_handle.h"
#include "world/model/role.h"
#include "world/rpc/world_msg.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace yuan::game::server
{
    class WorldServerService final : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
    {
    public:
        explicit WorldServerService(ServiceServerConfig config);

        void set_runtime_context(const yuan::app::RuntimeContext &context) override;
        bool init() override;
        void start() override;
        void stop() override;

        [[nodiscard]] bool ok() const;

    private:
        void register_http_routes();
        void ensure_player_roles(PlayerUid player_uid);
        yuan::coroutine::Task<void> ensure_player_roles_async(PlayerUid player_uid);
        void ensure_player_roles_async_callback(PlayerUid player_uid, std::function<void()> done);
        [[nodiscard]] RoleCache::LoadedPlayerRoles load_player_roles_from_db(PlayerUid player_uid) const;
        [[nodiscard]] std::optional<SSPlayerRoleInfo> create_role(PlayerUid player_uid, std::string name);
        void mark_role_dirty(PlayerId player_id, PackedGameServiceId zone_service_id);
        bool save_player_roles_to_db(PlayerUid player_uid, const std::vector<SSPlayerRoleInfo> &roles) const;
        bool save_role_location_to_db(PlayerId player_id, PackedGameServiceId zone_service_id) const;
        [[nodiscard]] PlayerUid player_uid_for_role(PlayerId player_id) const;
        [[nodiscard]] std::optional<SSGmCommandResponse> forward_gm(SSGmCommandRequest request) const;
        void flush_dirty_roles();
        void flush_loop(std::stop_token stop_token);
        void log_metrics() const;

        std::string listen_host_;
        std::uint16_t port_ = 0;
        std::uint16_t http_port_ = 0;
        bool ok_ = false;
        yuan::app::RuntimeContext context_;
        GameServiceId service_id_;
        std::uint16_t redis_flush_interval_ms_ = 5000;
        std::string world_ownership_store_ = "memory";
        std::atomic<bool> draining_{false};
        std::uint64_t metrics_log_interval_ms_ = 0;
        GameServiceId player_db_proxy_service_id_;
        DbProxyRoutingConfig player_db_proxy_routing_;
        DbProxyRoutingConfig world_db_proxy_routing_;
        mutable std::mutex pending_role_locations_mutex_;
        std::unordered_map<PlayerId, PackedGameServiceId> pending_role_locations_;
        WorldMsgContext world_context_;
        yuan::rpc::Server world_rpc_;
        RoleCache roles_;
        std::jthread flush_thread_;
        yuan::timer::TimerHandle metrics_timer_;
        mutable TunnelClientManager tunnel_client_manager_;
        rpc_network::RpcNetworkServer rpc_server_;
        std::unique_ptr<yuan::net::http::HttpServer> http_server_;
    };
}

#endif
