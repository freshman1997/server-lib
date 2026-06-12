#include "common/application_runtime.h"
#include "common/service_config.h"
#include "gateway/gateway_server_service.h"

#include <cstdlib>
#include <iostream>
#include <memory>

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::cerr << "usage: game_gateway_server <config-file>\n";
        return 2;
    }

    const auto config = yuan::game::server::load_service_server_config(argv[1]);
    if (!config) {
        std::cerr << "gateway failed to load config\n";
        return 2;
    }
    yuan::app::Application app(yuan::game::server::make_game_runtime_context("game.gateway"));
    auto service = std::make_shared<yuan::game::server::GatewayServerService>(config->service_id,
                                                                               config->listen_port,
                                                                               config->tunnel_port,
                                                                               config->target_world_id,
                                                                               config->expected_requests);
    if (!app.add_service("gateway", service) || !app.start()) {
        std::cerr << "gateway server failed to start\n";
        app.stop();
        return 3;
    }
    const bool ok = service->ok();
    app.stop();
    return ok ? EXIT_SUCCESS : 4;
}
