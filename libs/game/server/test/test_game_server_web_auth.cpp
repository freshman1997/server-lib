#include "web/handler/web_handler.h"

#include "http_server.h"

#include <nlohmann/json.hpp>

#include <iostream>

namespace
{
    bool require(bool condition, const char *message)
    {
        if (!condition) {
            std::cerr << message << '\n';
            return false;
        }
        return true;
    }
}

int main()
{
    using namespace yuan::game::server;

    WebHandlerContext web;
    LoginOptionsResponse options;
    options.gateways.push_back({pack_game_service_id(1, 7, GameServiceType::gateway, 1), "127.0.0.1", 30001, "gateway-a"});
    options.roles.push_back({10001, "hero", 9, pack_game_service_id(1, 7, GameServiceType::world, 1), 0});

    web.register_handler = [&options](WebAuthRequest request) {
        return WebAuthResponse{request.account == "alice" && request.password == "pw", 42, options, "registered"};
    };
    web.login_handler = [&options](WebAuthRequest request) {
        return WebAuthResponse{request.account == "alice" && request.password == "pw", 42, options, "login ok"};
    };
    const auto registered = web.register_handler(WebAuthRequest{"alice", "pw"});
    if (!require(registered.ok && registered.player_uid == 42, "register should return auth success")) {
        return 1;
    }
    const auto register_json = nlohmann::json::parse(encode_login_options_response_json(registered.login_options));
    if (!require(register_json["gateways"].size() == 1 && register_json["roles"].size() == 1,
                 "register should include login options json")) {
        return 2;
    }

    const auto logged_in = web.login_handler(WebAuthRequest{"alice", "pw"});
    if (!require(logged_in.ok && logged_in.login_options.roles.front().world_service_id != 0,
                 "login should return role bound to a world")) {
        return 3;
    }

    yuan::net::http::HttpServer http_server;
    if (!require(register_web_http_handlers(http_server, web), "web http handlers should register")) {
        return 4;
    }

    return 0;
}
