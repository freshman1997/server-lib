#ifndef YUAN_GAME_SERVER_WEB_WEB_SERVER_SERVICE_H
#define YUAN_GAME_SERVER_WEB_WEB_SERVER_SERVICE_H

#include "application.h"
#include "common/rpc_network.h"
#include "web/web_service.h"

#include <vector>

namespace yuan::redis
{
    class RedisClient;
}

namespace yuan::game::server
{
    class WebServerService final : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
    {
    public:
        WebServerService(GameServiceId service_id,
                          std::uint16_t port,
                          std::uint16_t tunnel_port,
                          GameServiceId world_id,
                          std::uint16_t world_port,
                          std::vector<std::uint16_t> world_ports,
                          std::string redis_host,
                          std::uint16_t redis_port,
                          std::uint16_t redis_db,
                          std::string redis_username,
                          std::string redis_password,
                          std::uint16_t redis_connect_timeout_ms,
                          std::uint16_t redis_command_timeout_ms,
                          std::size_t expected_requests = 1);

        void set_runtime_context(const yuan::app::RuntimeContext &context) override;
        bool init() override;
        void start() override;
        void stop() override;

        [[nodiscard]] bool ok() const;

    private:
        std::optional<LoginOptionsResponse> fetch_bootstrap(LoginOptionsRequest request) const;
        WebAuthResponse register_account(WebAuthRequest request) const;
        WebAuthResponse login_account(WebAuthRequest request) const;
        [[nodiscard]] std::uint16_t select_world_port(PlayerUid player_uid) const;
        [[nodiscard]] std::optional<LoginOptionsResponse> fetch_login_options(PlayerUid player_uid) const;

        std::uint16_t port_ = 0;
        std::uint16_t tunnel_port_ = 0;
        std::uint16_t world_port_ = 0;
        std::vector<std::uint16_t> world_ports_;
        std::string redis_host_;
        std::uint16_t redis_port_ = 6379;
        std::uint16_t redis_db_ = 0;
        std::string redis_username_;
        std::string redis_password_;
        std::uint16_t redis_connect_timeout_ms_ = 1000;
        std::uint16_t redis_command_timeout_ms_ = 1000;
        std::size_t expected_requests_ = 1;
        bool ok_ = false;
        yuan::app::RuntimeContext context_;
        GameServiceId service_id_;
        GameServiceId world_id_;
        WebService web_;
        std::shared_ptr<yuan::redis::RedisClient> redis_;
        rpc_network::RpcNetworkServer rpc_server_;
    };
}

#endif
