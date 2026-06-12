#include "common/application_runtime.h"
#include "common/service_config.h"
#include "zone/zone_process_service.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::cerr << "usage: game_zone_server_process <config-file>\n";
        return 2;
    }

    const auto config = yuan::game::server::load_service_process_config(argv[1]);
    if (!config) {
        std::cerr << "zone failed to load config\n";
        return 2;
    }

    yuan::app::Application app(yuan::game::server::make_game_runtime_context("game.zone"));
    auto service = std::make_shared<yuan::game::server::ZoneProcessService>(config->service_id,
                                                                           config->target_global_id,
                                                                           config->tunnel_port,
                                                                           config->listen_port);
    if (!app.add_service("zone", service) || !app.start()) {
        std::cerr << "zone application failed to start\n";
        app.stop();
        return 3;
    }
    const bool ok = service->ok();
    app.stop();
    return ok ? EXIT_SUCCESS : 4;
}
