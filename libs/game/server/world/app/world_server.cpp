#include "common/application_runtime.h"
#include "common/service_config.h"
#include "world/app/world_server_service.h"

#include "logger.h"

#include <cstdlib>
#include <memory>

int main(int argc, char **argv)
{
    if (argc != 2) {
        LOG_ERROR("usage: game_world_server <config-file>");
        return 2;
    }

    const auto config = yuan::game::server::load_service_server_config(argv[1]);
    if (!config) {
        LOG_ERROR("world failed to load config path={}", argv[1]);
        return 2;
    }
    yuan::app::Application app(yuan::game::server::make_game_runtime_context("game.world"));
    auto service = std::make_shared<yuan::game::server::WorldServerService>(config->service_id,
                                                                                config->listen_host,
                                                                                config->listen_port,
                                                                                config->http_port,
                                                                                 config->tunnel_endpoints,
                                                                              config->redis_host,
                                                                              config->redis_port,
                                                                              config->redis_db,
                                                                              config->redis_username,
                                                                                config->redis_password,
                                                                                   config->redis_connect_timeout_ms,
                                                                                   config->redis_command_timeout_ms,
                                                                                   config->redis_flush_interval_ms,
                                                                                   config->world_ownership_store,
                                                                                   config->login_reservation_ttl_ms,
                                                                                   config->zone_report_ttl_ms,
                                                                                   config->tunnel_heartbeat_interval_ms,
                                                                                   config->metrics_log_interval_ms);
    if (!app.add_service("world", service) || !app.start()) {
        LOG_ERROR("world server failed to start");
        app.stop();
        return 3;
    }
    const bool ok = service->ok();
    app.stop();
    return ok ? EXIT_SUCCESS : 4;
}
