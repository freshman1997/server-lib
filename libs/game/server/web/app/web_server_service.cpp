#include "web/app/web_server_service.h"

#include "internal/def.h"

namespace yuan::game::server
{
    WebServerService::WebServerService(std::string listen_host,
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
                                          std::uint16_t redis_command_timeout_ms)
        : listen_host_(std::move(listen_host)),
          port_(port),
          world_host_(std::move(world_host)),
          world_port_(world_port),
          world_ports_(std::move(world_ports)),
          world_endpoints_(std::move(world_endpoints)),
          world_routing_(std::move(world_routing)),
          redis_host_(std::move(redis_host)),
          redis_port_(redis_port),
          redis_db_(redis_db),
          redis_username_(std::move(redis_username)),
          redis_password_(std::move(redis_password)),
          redis_connect_timeout_ms_(redis_connect_timeout_ms),
          redis_command_timeout_ms_(redis_command_timeout_ms)
    {
    }

    void WebServerService::set_runtime_context(const yuan::app::RuntimeContext &context)
    {
        context_ = context;
    }

    bool WebServerService::init()
    {
        auth_service_ = std::make_unique<WebAuthService>(world_host_,
                                                         world_port_,
                                                         world_ports_,
                                                         world_endpoints_,
                                                         world_routing_,
                                                         redis_host_,
                                                         redis_port_,
                                                         redis_db_,
                                                         redis_username_,
                                                         redis_password_,
                                                         redis_connect_timeout_ms_,
                                                         redis_command_timeout_ms_);
        if (!auth_service_->init()) {
            return false;
        }
        web_context_.bootstrap_provider = [this](LoginOptionsRequest request) {
            return auth_service_->fetch_bootstrap(request);
        };
        web_context_.register_handler = [this](WebAuthRequest request) {
            return auth_service_->register_account(std::move(request));
        };
        web_context_.login_handler = [this](WebAuthRequest request) {
            return auth_service_->login_account(std::move(request));
        };
        yuan::net::http::HttpServerConfig http_config;
        http_config.enable_keep_alive = false;
        http_config.server_name = "GameWeb/1.0";
        http_server_ = std::make_unique<yuan::net::http::HttpServer>(http_config);
        if (!register_web_http_handlers(*http_server_, web_context_)) {
            return false;
        }
        ok_ = http_server_->init(port_);
        return ok_;
    }

    void WebServerService::start()
    {
        if (ok_ && http_server_) {
            http_thread_ = std::jthread([this](std::stop_token) {
                http_server_->serve();
            });
        }
    }

    void WebServerService::stop()
    {
        if (http_server_) {
            http_server_->stop();
        }
        if (http_thread_.joinable()) {
            http_thread_.join();
        }
    }

    bool WebServerService::ok() const
    {
        return ok_;
    }
}
