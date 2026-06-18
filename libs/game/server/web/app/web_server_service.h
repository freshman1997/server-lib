#ifndef YUAN_GAME_SERVER_WEB_WEB_SERVER_SERVICE_H
#define YUAN_GAME_SERVER_WEB_WEB_SERVER_SERVICE_H

#include "application.h"
#include "common/world_routing.h"
#include "web/handler/web_handler.h"
#include "web/service/web_auth_service.h"

#include "http_server.h"

#include <memory>
#include <vector>

namespace yuan::game::server
{
    class WebServerService final : public yuan::app::Service, public yuan::app::RuntimeContextAwareService
    {
    public:
        WebServerService(std::string listen_host,
                          std::uint16_t port,
                          std::string world_host,
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

        void set_runtime_context(const yuan::app::RuntimeContext &context) override;
        bool init() override;
        void start() override;
        void stop() override;

        [[nodiscard]] bool ok() const;

    private:
        std::string listen_host_;
        std::uint16_t port_ = 0;
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
        bool ok_ = false;
        yuan::app::RuntimeContext context_;
        WebHandlerContext web_context_;
        std::unique_ptr<WebAuthService> auth_service_;
        std::unique_ptr<yuan::net::http::HttpServer> http_server_;
    };
}

#endif
