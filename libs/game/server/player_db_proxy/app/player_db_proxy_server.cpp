#include "common/application_runtime.h"
#include "common/service_config.h"
#include "player_db_proxy/app/player_db_proxy_server_service.h"

#include "logger.h"

#include <cstdlib>
#include <memory>

int main(int argc, char **argv)
{
    if (argc != 2) {
        LOG_ERROR("usage: game_player_db_proxy_server <config-file>");
        return 2;
    }

    const auto config = yuan::game::server::load_service_server_config(argv[1]);
    if (!config) {
        LOG_ERROR("player_db_proxy failed to load config path={}", argv[1]);
        return 2;
    }

    yuan::app::Application app(yuan::game::server::make_game_runtime_context("game.player_db_proxy"));
    auto service = std::make_shared<yuan::game::server::PlayerDbProxyServerService>(*config);
    if (!app.add_service("player_db_proxy", service) || !app.start()) {
        LOG_ERROR("player_db_proxy server failed to start");
        app.stop();
        return 3;
    }
    
    const bool ok = service->ok();
    app.stop();
    return ok ? EXIT_SUCCESS : 4;
}
