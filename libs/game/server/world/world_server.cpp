#include "common/application_runtime.h"
#include "common/service_config.h"
#include "world/world_server_service.h"

#include <cstdlib>
#include <iostream>
#include <memory>

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::cerr << "usage: game_world_server <config-file>\n";
        return 2;
    }

    const auto config = yuan::game::server::load_service_server_config(argv[1]);
    if (!config) {
        std::cerr << "world failed to load config\n";
        return 2;
    }
    yuan::app::Application app(yuan::game::server::make_game_runtime_context("game.world"));
    auto service = std::make_shared<yuan::game::server::WorldServerService>(config->service_id,
                                                                              config->listen_port,
                                                                              config->tunnel_port,
                                                                              config->redis_host,
                                                                              config->redis_port,
                                                                              config->redis_db,
                                                                              config->redis_username,
                                                                              config->redis_password,
                                                                              config->redis_connect_timeout_ms,
                                                                              config->redis_command_timeout_ms,
                                                                              config->redis_flush_interval_ms,
                                                                              config->expected_requests);
    if (!app.add_service("world", service) || !app.start()) {
        std::cerr << "world server failed to start\n";
        app.stop();
        return 3;
    }
    const bool ok = service->ok();
    app.stop();
    return ok ? EXIT_SUCCESS : 4;
}
