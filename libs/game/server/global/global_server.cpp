#include "common/application_runtime.h"
#include "common/service_config.h"
#include "global/global_server_service.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::cerr << "usage: game_global_server <config-file>\n";
        return 2;
    }

    const auto config = yuan::game::server::load_service_server_config(argv[1]);
    if (!config) {
        std::cerr << "global failed to load config\n";
        return 2;
    }
    yuan::app::Application app(yuan::game::server::make_game_runtime_context("game.global"));
    auto service = std::make_shared<yuan::game::server::GlobalServerService>(config->service_id, config->listen_port, config->tunnel_port);
    if (!app.add_service("global", service) || !app.start()) {
        std::cerr << "global server failed to start\n";
        app.stop();
        return 3;
    }
    const bool ok = service->ok();
    app.stop();
    return ok ? EXIT_SUCCESS : 4;
}
