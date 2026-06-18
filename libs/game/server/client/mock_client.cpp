#include "common/rpc_network.h"
#include "common/codec/game_binary_codec.h"

#include "base/time.h"
#include "coroutine/sync_wait.h"
#include "http_client.h"
#include "net/async/async_request_client.h"
#include "request.h"
#include "response.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace
{
    yuan::buffer::ByteBuffer to_buffer(const yuan::rpc::Bytes &bytes)
    {
        yuan::buffer::ByteBuffer buffer(bytes.size());
        if (!bytes.empty()) {
            buffer.append(bytes.data(), bytes.size());
        }
        return buffer;
    }

    yuan::rpc::Bytes to_bytes(const yuan::buffer::ByteBuffer &buffer)
    {
        const auto span = buffer.readable_span();
        const auto *data = reinterpret_cast<const std::uint8_t *>(span.data());
        return yuan::rpc::Bytes(data, data + span.size());
    }

    class PersistentRpcClient
    {
    public:
        PersistentRpcClient()
            : client_(runtime_.runtime_view())
        {
        }

        bool connect(const std::string &host, std::uint16_t port)
        {
            auto rv = runtime_.runtime_view();
            auto task = [&](yuan::coroutine::RuntimeView view) -> yuan::coroutine::Task<bool> {
                co_await view.schedule();
                co_return co_await client_.connect_async(host, port, 1000);
            };
            return yuan::coroutine::sync_wait(rv, task(rv));
        }

        std::optional<yuan::rpc::Response> call(const yuan::rpc::Message &message)
        {
            auto rv = runtime_.runtime_view();
            auto task = [&](yuan::coroutine::RuntimeView view) -> yuan::coroutine::Task<std::optional<yuan::rpc::Response>> {
                co_await view.schedule();
                yuan::rpc::Bytes request_frame;
                if (!yuan::rpc::wire::encode_message(message, request_frame)) {
                    co_return std::nullopt;
                }
                const auto request_buffer = to_buffer(request_frame);
                const auto read_result = co_await client_.request_async(request_buffer, 1000);
                if (read_result.status != yuan::coroutine::IoStatus::success) {
                    co_return std::nullopt;
                }
                auto decoded = yuan::rpc::wire::decode_frame(to_bytes(read_result.data));
                if (!decoded.ok) {
                    co_return std::nullopt;
                }
                co_return yuan::rpc::wire::to_response(std::move(decoded.frame));
            };
            return yuan::coroutine::sync_wait(rv, task(rv));
        }

        void disconnect()
        {
            client_.disconnect();
        }

    private:
        yuan::net::NetworkRuntime runtime_;
        yuan::net::AsyncRequestClient client_;
    };

    int run_logout(PersistentRpcClient &gateway_client,
                   yuan::game::server::PlayerUid player_uid,
                   yuan::game::server::RoleId role_id)
    {
        yuan::rpc::Bytes logout_payload;
    if (!yuan::game::server::encode_binary(yuan::game::server::CSLoginRequest{player_uid, role_id, {}, 0}, logout_payload)) {
            std::cerr << "failed to encode logout request\n";
            return 16;
        }
        yuan::rpc::Message logout_message;
        logout_message.route = yuan::game::server::game_route::gateway_logout();
        logout_message.payload = std::move(logout_payload);
        const auto logout_response = gateway_client.call(logout_message);
        if (!logout_response || logout_response->status != yuan::rpc::RpcStatus::ok) {
            std::cerr << "gateway logout failed status="
                      << (logout_response ? static_cast<int>(logout_response->status) : -1)
                      << " error=" << (logout_response ? logout_response->error : "no response") << "\n";
            return 17;
        }
        const auto logout = yuan::game::server::decode_binary<yuan::game::server::CSLoginResponse>(logout_response->payload);
        if (!logout || !logout->ok) {
            std::cerr << "invalid logout response\n";
            return 18;
        }
        std::cout << "logout ok role=" << logout->role_id << "\n";
        return EXIT_SUCCESS;
    }

    std::optional<yuan::game::server::LoginOptionsResponse> fetch_login_options(std::uint16_t world_http_port,
                                                                                yuan::game::server::PlayerUid player_uid)
    {
        yuan::net::NetworkRuntime runtime;
        yuan::net::http::HttpClient http_client;
        if (!http_client.query("http://127.0.0.1:" + std::to_string(world_http_port))) {
            std::cerr << "failed to initialize world http client\n";
            return std::nullopt;
        }
        auto *http_response = yuan::coroutine::sync_wait(runtime.runtime_view(), http_client.connect_async(runtime.runtime_view(), [player_uid](yuan::net::http::HttpRequest *request) {
            request->set_method(yuan::net::http::HttpMethod::get_);
            request->set_raw_url("/game/login_options?player_uid=" + std::to_string(player_uid));
            request->add_header("Connection", "close");
            request->add_header("Host", "127.0.0.1");
            request->send();
        }));
        if (!http_response || !http_response->good() || http_response->get_response_code() != yuan::net::http::ResponseCode::ok_) {
            std::cerr << "world http login options failed response=" << (http_response != nullptr)
                      << " good=" << (http_response ? http_response->good() : false)
                      << " code=" << (http_response ? static_cast<int>(http_response->get_response_code()) : -1) << "\n";
            return std::nullopt;
        }
        const auto *body_begin = http_response->body_begin();
        const std::string body = body_begin ? std::string(body_begin, http_response->get_body_length()) : std::string();
        return yuan::game::server::decode_login_options_response_json(body);
    }

    bool create_role(std::uint16_t world_http_port, yuan::game::server::PlayerUid player_uid)
    {
        yuan::net::NetworkRuntime runtime;
        yuan::net::http::HttpClient http_client;
        if (!http_client.query("http://127.0.0.1:" + std::to_string(world_http_port))) {
            std::cerr << "failed to initialize world create role client\n";
            return false;
        }
        auto *http_response = yuan::coroutine::sync_wait(runtime.runtime_view(), http_client.connect_async(runtime.runtime_view(), [player_uid](yuan::net::http::HttpRequest *request) {
            request->set_method(yuan::net::http::HttpMethod::get_);
            request->set_raw_url("/game/create_role?player_uid=" + std::to_string(player_uid) + "&name=SmokeRole");
            request->add_header("Connection", "close");
            request->add_header("Host", "127.0.0.1");
            request->send();
        }));
        if (!http_response || !http_response->good() || http_response->get_response_code() != yuan::net::http::ResponseCode::ok_) {
            const auto *body_begin = http_response ? http_response->body_begin() : nullptr;
            const std::string body = body_begin ? std::string(body_begin, http_response->get_body_length()) : std::string();
            std::cerr << "world create role failed response=" << (http_response != nullptr)
                      << " good=" << (http_response ? http_response->good() : false)
                      << " code=" << (http_response ? static_cast<int>(http_response->get_response_code()) : -1)
                      << " body=" << body << "\n";
            return false;
        }
        return true;
    }
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        std::cerr << "usage: game_mock_client <world-http-port> <world-rpc-port> <player-uid>\n";
        return 2;
    }

    const auto world_http_port = static_cast<std::uint16_t>(std::stoul(argv[1]));
    const auto world_rpc_port = static_cast<std::uint16_t>(std::stoul(argv[2]));
    const auto player_uid = static_cast<yuan::game::server::PlayerUid>(std::stoull(argv[3]));

    auto options = fetch_login_options(world_http_port, player_uid);
    if (!options) {
        std::cerr << "failed to decode world login options\n";
        return 5;
    }
    if (options->roles.empty()) {
        std::cout << "world options initially have no roles; creating role\n";
        if (!create_role(world_http_port, player_uid)) {
            return 4;
        }
        options = fetch_login_options(world_http_port, player_uid);
        if (!options) {
            std::cerr << "failed to decode world login options after create role\n";
            return 5;
        }
    }

    std::cout << "world options ok uid=" << player_uid << " gateways=" << options->gateways.size() << " roles=" << options->roles.size() << " zones=" << options->zones.size() << "\n";
    for (const auto &gateway : options->gateways) {
        std::cout << "gateway " << gateway.name << " " << gateway.host << ":" << gateway.port << "\n";
    }
    for (const auto &role : options->roles) {
        std::cout << "role " << role.role_id << " " << role.name << " level=" << role.level << "\n";
    }
    for (const auto &zone : options->zones) {
        std::cout << "zone " << zone.name << " online=" << zone.online_players << " max=" << zone.max_players << " available=" << zone.available << "\n";
    }
    if (options->gateways.empty() || options->roles.empty() || options->zones.empty()) {
        return 6;
    }

    PersistentRpcClient gateway_client;
    if (!gateway_client.connect(options->gateways.front().host, options->gateways.front().port)) {
        std::cerr << "failed to connect persistent gateway rpc client\n";
        return 22;
    }

    yuan::rpc::Bytes login_payload;
    if (!yuan::game::server::encode_binary(yuan::game::server::CSLoginRequest{player_uid, options->roles.front().role_id, options->roles.front().login_token_id, 0}, login_payload)) {
        std::cerr << "failed to encode login request\n";
        return 7;
    }
    yuan::rpc::Message login_message;
    login_message.route = yuan::game::server::game_route::gateway_login();
    login_message.payload = std::move(login_payload);
    const auto login_response = gateway_client.call(login_message);
    if (!login_response || login_response->status != yuan::rpc::RpcStatus::ok) {
        std::cerr << "gateway login failed status=" << (login_response ? static_cast<int>(login_response->status) : -1)
                  << " error=" << (login_response ? login_response->error : "missing response") << "\n";
        return 8;
    }
    const auto login = yuan::game::server::decode_binary<yuan::game::server::CSLoginResponse>(login_response->payload);
    if (!login || !login->ok) {
        std::cerr << "login rejected status=" << static_cast<int>(login_response->status)
                  << " error=" << login_response->error
                  << " message=" << (login ? login->message : "decode failed")
                  << "\n";
        return 9;
    }
    std::cout << "login ok role=" << login->role_id << " message=" << login->message << "\n";

    yuan::rpc::Bytes gm_payload;
    if (!yuan::game::server::encode_binary(
            yuan::game::server::SSGmCommandRequest{0,
                                                  "set_player_level",
                                                  {std::to_string(login->role_id), "9"}},
            gm_payload)) {
        std::cerr << "failed to encode gm request\n";
        return 19;
    }
    yuan::rpc::Message gm_message;
    gm_message.route = yuan::game::server::game_route::world_gm_forward();
    gm_message.payload = std::move(gm_payload);
    const auto gm_response = yuan::game::server::rpc_network::RpcNetworkClient().call(
        yuan::game::server::rpc_network::RpcEndpoint{"127.0.0.1", world_rpc_port}, gm_message);
    if (!gm_response || gm_response->status != yuan::rpc::RpcStatus::ok) {
        std::cerr << "world gm forward failed status="
                  << (gm_response ? static_cast<int>(gm_response->status) : -1)
                  << " error=" << (gm_response ? gm_response->error : "no response") << "\n";
        return 20;
    }
    const auto gm = yuan::game::server::decode_binary<yuan::game::server::SSGmCommandResponse>(gm_response->payload);
    if (!gm || !gm->ok) {
        std::cerr << "gm set player level failed message=" << (gm ? gm->message : "decode failed") << "\n";
        return 21;
    }
    std::cout << "gm ok " << gm->message << "\n";

    yuan::rpc::Bytes game_payload;
    if (!yuan::game::server::encode_binary(
            yuan::game::server::CSGameRequest{player_uid,
                                                    login->role_id,
                                                    0,
                                                    yuan::rpc::Codec<std::string>::encode("move:1,2")},
            game_payload)) {
        std::cerr << "failed to encode game request\n";
        return 10;
    }
    yuan::rpc::Message game_message;
    game_message.route = yuan::game::server::game_route::gateway_game_forward();
    game_message.payload = std::move(game_payload);
    const auto game_response = gateway_client.call(game_message);
    if (!game_response || game_response->status != yuan::rpc::RpcStatus::ok) {
        std::cerr << "gateway game forward failed\n";
        return 11;
    }
    const auto game = yuan::game::server::decode_binary<yuan::game::server::CSGameResponse>(game_response->payload);
    if (!game || !game->ok || yuan::rpc::Codec<std::string>::decode(game->payload) != "move:1,2") {
        std::cerr << "game response mismatch\n";
        return 12;
    }
    std::cout << "game ok role=" << game->role_id << " payload=" << yuan::rpc::Codec<std::string>::decode(game->payload) << "\n";

    const auto client_time_seconds = yuan::base::time::system_now_sec();
    yuan::rpc::Bytes time_sync_payload;
    if (!yuan::game::server::encode_binary(yuan::game::server::CSTimeSyncRequest{player_uid, login->role_id, 0, client_time_seconds}, time_sync_payload)) {
        std::cerr << "failed to encode time sync request\n";
        return 13;
    }
    yuan::rpc::Message time_sync_message;
    time_sync_message.route = yuan::game::server::game_route::gateway_time_sync();
    time_sync_message.payload = std::move(time_sync_payload);
    const auto time_sync_response = gateway_client.call(time_sync_message);
    if (!time_sync_response || time_sync_response->status != yuan::rpc::RpcStatus::ok) {
        std::cerr << "gateway time sync failed\n";
        return 14;
    }
    const auto time_sync = yuan::game::server::decode_binary<yuan::game::server::CSTimeSyncResponse>(time_sync_response->payload);
    if (!time_sync || !time_sync->ok || time_sync->client_time_seconds != client_time_seconds || time_sync->server_receive_time_seconds == 0 ||
        time_sync->server_send_time_seconds < time_sync->server_receive_time_seconds) {
        std::cerr << "invalid time sync response\n";
        return 15;
    }
    std::cout << "time sync ok server_recv=" << time_sync->server_receive_time_seconds << " server_send=" << time_sync->server_send_time_seconds << "\n";

    const auto logout_status = run_logout(gateway_client, player_uid, login->role_id);
    if (logout_status != EXIT_SUCCESS) {
        return logout_status;
    }
    gateway_client.disconnect();

#ifndef _WIN32
    std::cout.flush();
    std::cerr.flush();
    _exit(EXIT_SUCCESS);
#endif

    return EXIT_SUCCESS;
}
