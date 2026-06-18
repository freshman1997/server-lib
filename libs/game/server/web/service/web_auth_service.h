#ifndef YUAN_GAME_SERVER_WEB_SERVICE_WEB_AUTH_SERVICE_H
#define YUAN_GAME_SERVER_WEB_SERVICE_WEB_AUTH_SERVICE_H

#include "common/codec/game_binary_codec.h"
#include "common/world_routing.h"
#include "web/service/web_auth_types.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace yuan::redis
{
    class RedisClient;
}

namespace yuan::game::server
{
    class WebAuthService
    {
    public:
        WebAuthService(std::string world_host,
                       std::uint16_t world_port,
                       std::vector<std::uint16_t> world_ports,
                       std::vector<WorldEndpointConfig> world_endpoints,
                       WorldRoutingConfig world_routing,
                       std::string redis_host,
                       std::uint16_t redis_port,
                       std::uint16_t redis_db,
                       std::string redis_username,
                       std::string redis_password,
                       std::uint16_t redis_connect_timeout_ms,
                       std::uint16_t redis_command_timeout_ms);

        bool init();

        std::optional<LoginOptionsResponse> fetch_bootstrap(LoginOptionsRequest request) const;
        WebAuthResponse register_account(WebAuthRequest request) const;
        WebAuthResponse login_account(WebAuthRequest request) const;
        WebCreateRoleResponse create_role(WebCreateRoleRequest request) const;

    private:
        [[nodiscard]] std::uint16_t select_world_port(PlayerUid player_uid) const;
        [[nodiscard]] std::optional<LoginOptionsResponse> fetch_login_options(PlayerUid player_uid) const;

        std::string world_host_;
        std::uint16_t world_port_ = 0;
        std::vector<std::uint16_t> world_ports_;
        std::vector<WorldEndpointConfig> world_endpoints_;
        WorldRoutingConfig world_routing_;
        std::string redis_host_;
        std::uint16_t redis_port_ = 6379;
        std::uint16_t redis_db_ = 0;
        std::string redis_username_;
        std::string redis_password_;
        std::uint16_t redis_connect_timeout_ms_ = 1000;
        std::uint16_t redis_command_timeout_ms_ = 1000;
        std::shared_ptr<yuan::redis::RedisClient> redis_;
    };
}

#endif
