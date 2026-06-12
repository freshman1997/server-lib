#include "web/web_service.h"

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

    WebService web({{1, 7, GameServiceType::web, 7, 1}, 600, yuan::game_base::ServerRole::world, 7, "web"});
    LoginOptionsResponse options;
    options.gateways.push_back({pack_game_service_id(1, 7, GameServiceType::gateway, 1), "127.0.0.1", 30001, "gateway-a"});
    options.roles.push_back({10001, "hero", 9, pack_game_service_id(1, 7, GameServiceType::world, 1), 0});

    web.set_register_handler([&options](WebAuthRequest request) {
        return WebAuthResponse{request.account == "alice" && request.password == "pw", 42, options, "registered"};
    });
    web.set_login_handler([&options](WebAuthRequest request) {
        return WebAuthResponse{request.account == "alice" && request.password == "pw", 42, options, "login ok"};
    });

    yuan::rpc::Bytes payload;
    if (!require(encode_web_auth_request({"alice", "pw"}, payload), "auth request should encode")) {
        return 1;
    }

    yuan::rpc::Message register_message;
    register_message.route = game_route::web_register();
    register_message.payload = payload;
    const auto register_response = web.rpc_server().handle(register_message);
    if (!require(register_response.status == yuan::rpc::RpcStatus::ok, "register should return ok status")) {
        return 2;
    }
    const auto registered = decode_web_auth_response(register_response.payload);
    if (!require(registered && registered->ok && registered->player_uid == 42, "register should return auth success")) {
        return 3;
    }
    if (!require(registered->login_options.gateways.size() == 1 && registered->login_options.roles.size() == 1,
                 "register should include login options")) {
        return 4;
    }

    yuan::rpc::Message login_message;
    login_message.route = game_route::web_login();
    login_message.payload = std::move(payload);
    const auto login_response = web.rpc_server().handle(login_message);
    if (!require(login_response.status == yuan::rpc::RpcStatus::ok, "login should return ok status")) {
        return 5;
    }
    const auto logged_in = decode_web_auth_response(login_response.payload);
    if (!require(logged_in && logged_in->ok && logged_in->login_options.roles.front().world_service_id != 0,
                 "login should return role bound to a world")) {
        return 6;
    }

    return 0;
}
