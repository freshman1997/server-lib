#include "common/application_runtime.h"
#include "common/service_config.h"
#include "gateway/app/gateway_server_service.h"

#include "logger.h"

#include <cstdlib>
#include <memory>

int main(int argc, char **argv)
{
    if (argc != 2) {
        LOG_ERROR("usage: game_gateway_server <config-file>");
        return 2;
    }

    const auto config = yuan::game::server::load_service_server_config(argv[1]);
    if (!config) {
        LOG_ERROR("gateway failed to load config path={}", argv[1]);
        return 2;
    }
    yuan::app::Application app(yuan::game::server::make_game_runtime_context("game.gateway"));
    yuan::game::server::rpc_network::RpcNetworkServerConfig rpc_server_config;
    rpc_server_config.max_connections = static_cast<std::size_t>(config->rpc_max_connections);
    rpc_server_config.max_buffered_bytes = static_cast<std::size_t>(config->rpc_max_buffered_bytes);
    rpc_server_config.idle_timeout_ms = config->rpc_idle_timeout_ms == 0 ? 10 * 60 * 1000 : config->rpc_idle_timeout_ms;

    auto service = std::make_shared<yuan::game::server::GatewayServerService>(config->service_id,
                                                                                   config->listen_host,
                                                                                   config->listen_port,
                                                                                   config->public_host,
                                                                                   config->zone_endpoints,
                                                                                      config->metrics_log_interval_ms,
                                                                                     rpc_server_config,
                                                                                     config->login_token_secret,
                                                                                     config->gateway_internal_secret,
                                                                                     config->gateway_drain_timeout_ms);
    if (!app.add_service("gateway", service) || !app.start()) {
        LOG_ERROR("gateway server failed to start");
        app.stop();
        return 3;
    }
    const bool ok = service->ok();
    app.stop();
    return ok ? EXIT_SUCCESS : 4;
}
