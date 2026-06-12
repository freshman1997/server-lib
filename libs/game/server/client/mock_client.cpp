#include "common/rpc_network.h"
#include "common/game_messages.h"

#include "base/time.h"

#include <cstdlib>
#include <iostream>

int main(int argc, char **argv)
{
    if (argc != 3) {
        std::cerr << "usage: game_mock_client <web-port> <player-uid>\n";
        return 2;
    }

    const auto web_port = static_cast<std::uint16_t>(std::stoul(argv[1]));
    const auto player_uid = static_cast<yuan::game::server::PlayerUid>(std::stoull(argv[2]));

    const std::string account = "mock-" + std::to_string(player_uid);
    const std::string password = "pwd";

    yuan::rpc::Bytes auth_payload;
    if (!yuan::game::server::encode_web_auth_request({account, password}, auth_payload)) {
        std::cerr << "failed to encode register request\n";
        return 3;
    }
    yuan::rpc::Message auth_message;
    auth_message.route = yuan::game::server::game_route::web_register();
    auth_message.payload = auth_payload;
    auto auth_response = yuan::game::server::rpc_network::RpcNetworkClient().call(
        yuan::game::server::rpc_network::RpcEndpoint{"127.0.0.1", web_port}, auth_message);
    auto auth = auth_response ? yuan::game::server::decode_web_auth_response(auth_response->payload) : std::nullopt;
    auto register_response = auth_response;
    auto register_auth = auth;
    if (!auth || !auth->ok) {
        auth_message.route = yuan::game::server::game_route::web_login();
        auth_message.payload = std::move(auth_payload);
        auth_response = yuan::game::server::rpc_network::RpcNetworkClient().call(
            yuan::game::server::rpc_network::RpcEndpoint{"127.0.0.1", web_port}, auth_message);
        auth = auth_response ? yuan::game::server::decode_web_auth_response(auth_response->payload) : std::nullopt;
    }
    if (!auth_response || auth_response->status != yuan::rpc::RpcStatus::ok) {
        std::cerr << "web auth failed status="
                  << (auth_response ? static_cast<int>(auth_response->status) : -1)
                  << " error=" << (auth_response ? auth_response->error : "no response") << "\n";
        std::cerr << "register status="
                  << (register_response ? static_cast<int>(register_response->status) : -1)
                  << " error=" << (register_response ? register_response->error : "no response") << "\n";
        if (register_auth) {
            std::cerr << "register auth ok=" << register_auth->ok << " message=" << register_auth->message
                      << " uid=" << register_auth->player_uid << "\n";
        }
        if (auth) {
            std::cerr << "auth ok=" << auth->ok << " message=" << auth->message << " uid=" << auth->player_uid << "\n";
        }
        return 4;
    }
    if (!auth || !auth->ok) {
        std::cerr << "failed auth response ok=" << (auth ? auth->ok : false)
                  << " message=" << (auth ? auth->message : "decode failed") << "\n";
        return 5;
    }
    const auto &options = auth->login_options;

    std::cout << "auth ok uid=" << auth->player_uid << " gateways=" << options.gateways.size() << " roles=" << options.roles.size() << "\n";
    for (const auto &gateway : options.gateways) {
        std::cout << "gateway " << gateway.name << " " << gateway.host << ":" << gateway.port << " service=" << gateway.service_id << "\n";
    }
    for (const auto &role : options.roles) {
        std::cout << "role " << role.role_id << " " << role.name << " level=" << role.level << " world=" << role.world_service_id << " zone=" << role.zone_service_id << "\n";
    }
    if (options.gateways.empty() || options.roles.empty()) {
        return 6;
    }

    yuan::rpc::Bytes login_payload;
    if (!yuan::game::server::encode_client_login_request({auth->player_uid, options.roles.front().role_id}, login_payload)) {
        std::cerr << "failed to encode login request\n";
        return 7;
    }
    yuan::rpc::Message login_message;
    login_message.route = yuan::game::server::game_route::gateway_login();
    login_message.payload = std::move(login_payload);
    const auto login_response = yuan::game::server::rpc_network::RpcNetworkClient().call(
        yuan::game::server::rpc_network::RpcEndpoint{options.gateways.front().host, options.gateways.front().port}, login_message);
    if (!login_response || login_response->status != yuan::rpc::RpcStatus::ok) {
        std::cerr << "gateway login failed\n";
        return 8;
    }
    const auto login = yuan::game::server::decode_client_login_response(login_response->payload);
    if (!login || !login->ok) {
        std::cerr << "login rejected status=" << static_cast<int>(login_response->status)
                  << " error=" << login_response->error
                  << " message=" << (login ? login->message : "decode failed")
                  << " zone=" << (login ? login->zone_service_id : 0) << "\n";
        return 9;
    }
    std::cout << "login ok role=" << login->role_id << " zone=" << login->zone_service_id << " message=" << login->message << "\n";

    yuan::rpc::Bytes game_payload;
    if (!yuan::game::server::encode_client_game_request(
            yuan::game::server::ClientGameRequest{auth->player_uid,
                                                   login->role_id,
                                                   yuan::rpc::Codec<std::string>::encode("move:1,2")},
            game_payload)) {
        std::cerr << "failed to encode game request\n";
        return 10;
    }
    yuan::rpc::Message game_message;
    game_message.route = yuan::game::server::game_route::gateway_game_forward();
    game_message.payload = std::move(game_payload);
    const auto game_response = yuan::game::server::rpc_network::RpcNetworkClient().call(
        yuan::game::server::rpc_network::RpcEndpoint{options.gateways.front().host, options.gateways.front().port}, game_message);
    if (!game_response || game_response->status != yuan::rpc::RpcStatus::ok) {
        std::cerr << "gateway game forward failed\n";
        return 11;
    }
    const auto game = yuan::game::server::decode_client_game_response(game_response->payload);
    if (!game || !game->ok || yuan::rpc::Codec<std::string>::decode(game->payload) != "move:1,2") {
        std::cerr << "game response mismatch\n";
        return 12;
    }
    std::cout << "game ok role=" << game->role_id << " payload=" << yuan::rpc::Codec<std::string>::decode(game->payload) << "\n";

    const auto client_time_seconds = yuan::base::time::system_now_sec();
    yuan::rpc::Bytes time_sync_payload;
    if (!yuan::game::server::encode_client_time_sync_request({auth->player_uid, login->role_id, client_time_seconds}, time_sync_payload)) {
        std::cerr << "failed to encode time sync request\n";
        return 13;
    }
    yuan::rpc::Message time_sync_message;
    time_sync_message.route = yuan::game::server::game_route::gateway_time_sync();
    time_sync_message.payload = std::move(time_sync_payload);
    const auto time_sync_response = yuan::game::server::rpc_network::RpcNetworkClient().call(
        yuan::game::server::rpc_network::RpcEndpoint{options.gateways.front().host, options.gateways.front().port}, time_sync_message);
    if (!time_sync_response || time_sync_response->status != yuan::rpc::RpcStatus::ok) {
        std::cerr << "gateway time sync failed\n";
        return 14;
    }
    const auto time_sync = yuan::game::server::decode_client_time_sync_response(time_sync_response->payload);
    if (!time_sync || !time_sync->ok || time_sync->client_time_seconds != client_time_seconds || time_sync->server_receive_time_seconds == 0 ||
        time_sync->server_send_time_seconds < time_sync->server_receive_time_seconds) {
        std::cerr << "invalid time sync response\n";
        return 15;
    }
    std::cout << "time sync ok server_recv=" << time_sync->server_receive_time_seconds << " server_send=" << time_sync->server_send_time_seconds << "\n";

    yuan::rpc::Bytes logout_payload;
    if (!yuan::game::server::encode_client_login_request({auth->player_uid, login->role_id}, logout_payload)) {
        std::cerr << "failed to encode logout request\n";
        return 16;
    }
    yuan::rpc::Message logout_message;
    logout_message.route = yuan::game::server::game_route::gateway_logout();
    logout_message.payload = std::move(logout_payload);
    const auto logout_response = yuan::game::server::rpc_network::RpcNetworkClient().call(
        yuan::game::server::rpc_network::RpcEndpoint{options.gateways.front().host, options.gateways.front().port}, logout_message);
    if (!logout_response || logout_response->status != yuan::rpc::RpcStatus::ok) {
        std::cerr << "gateway logout failed\n";
        return 17;
    }
    const auto logout = yuan::game::server::decode_client_login_response(logout_response->payload);
    if (!logout || !logout->ok || logout->zone_service_id != 0) {
        std::cerr << "invalid logout response\n";
        return 18;
    }
    std::cout << "logout ok role=" << logout->role_id << "\n";
    return EXIT_SUCCESS;
}
