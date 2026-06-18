#include "chat_web/app/chat_web_server_service.h"
#include "common/application_runtime.h"
#include "common/service_config.h"

#include "logger.h"

#include <cstdlib>
#include <memory>

int main(int argc, char **argv)
{
    if (argc != 2) {
        LOG_ERROR("usage: game_chat_web_server <config-file>");
        return 2;
    }
    const auto config = yuan::game::server::load_service_server_config(argv[1]);
    if (!config) {
        LOG_ERROR("chat web failed to load config path={}", argv[1]);
        return 2;
    }
    yuan::app::Application app(yuan::game::server::make_game_runtime_context("game.chat_web"));
    auto service = std::make_shared<yuan::game::server::ChatWebServerService>(config->listen_host,
                                                                              config->listen_port,
                                                                              config->redis_host,
                                                                              config->redis_port,
                                                                              config->redis_db,
                                                                              config->redis_username,
                                                                              config->redis_password,
                                                                              config->redis_connect_timeout_ms,
                                                                              config->redis_command_timeout_ms);
    if (!app.add_service("chat_web", service) || !app.start()) {
        LOG_ERROR("chat web server failed to start");
        app.stop();
        return 3;
    }
    const bool ok = service->ok();
    app.stop();
    return ok ? EXIT_SUCCESS : 4;
}
