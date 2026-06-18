#include "common/codec/game_binary_codec.h"
#include "common/proto/player_db_proto.h"
#include "player_db_proxy/rpc/player_db_msg.h"

#include "option.h"
#include "redis_client.h"

#include <chrono>
#include <iostream>
#include <memory>

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

    yuan::rpc::Message make_message(yuan::rpc::Route route, const yuan::rpc::Bytes &payload)
    {
        yuan::rpc::Message message;
        message.request_id = 100;
        message.set_continuation_id(200);
        message.route = std::move(route);
        message.payload = payload;
        return message;
    }
}

int main()
{
    using namespace yuan::game::server;

    yuan::redis::Option option;
    option.host_ = "127.0.0.1";
    option.port_ = 6379;
    option.db_ = 0;
    option.timeout_ms_ = 500;
    option.connect_timeout_ms_ = 500;
    option.command_timeout_ms_ = 500;
    option.name_ = "game-player-db-proxy-test";

    auto redis = std::make_shared<yuan::redis::RedisClient>(option);
    if (!redis->ensure_connected() || !redis->ping()) {
        std::cerr << "game_server_player_db_proxy skipped: local Redis 127.0.0.1:6379 unavailable\n";
        return 0;
    }

    const auto suffix = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000);
    const PlayerUid player_uid = 880000 + suffix;
    const RoleId role_id = 990000 + suffix;

    PlayerDbMsgContext context;
    context.redis = redis;
    yuan::rpc::Server rpc;
    if (!require(register_player_db_msg(rpc, context), "player db handlers should register")) {
        return 1;
    }

    yuan::rpc::Bytes payload;
    (void)encode_binary(SSPlayerDbLoadRoleRequest{player_uid, role_id}, payload);
    auto response = rpc.handle(make_message(game_route::player_db_load_role(), payload));
    auto load_response = decode_binary<SSPlayerDbRoleResponse>(response.payload);
    if (!require(response.status == yuan::rpc::RpcStatus::ok && load_response && load_response->ok && !load_response->has_role,
                 "missing role should load as empty ok response")) {
        return 2;
    }

    SSPlayerDbSaveRoleRequest save_request;
    save_request.role = SSPlayerRoleData{player_uid, role_id, 15, 1234};
    save_request.data_version = 7;
    payload.clear();
    (void)encode_binary(save_request, payload);
    response = rpc.handle(make_message(game_route::player_db_save_role(), payload));
    auto save_response = decode_binary<SSPlayerDbRoleResponse>(response.payload);
    if (!require(response.status == yuan::rpc::RpcStatus::ok && save_response && save_response->has_role && save_response->data_version == 7,
                 "saved role should return versioned role data")) {
        return 3;
    }

    payload.clear();
    (void)encode_binary(SSPlayerDbLoadRoleRequest{player_uid, role_id}, payload);
    response = rpc.handle(make_message(game_route::player_db_load_role(), payload));
    load_response = decode_binary<SSPlayerDbRoleResponse>(response.payload);
    if (!require(response.status == yuan::rpc::RpcStatus::ok && load_response && load_response->has_role,
                 "saved role should load")) {
        return 4;
    }
    if (!require(load_response->role.level == 15 && load_response->role.exp == 1234 && load_response->data_version == 7,
                 "loaded role fields should roundtrip")) {
        return 5;
    }

    payload.clear();
    (void)encode_binary(SSPlayerDbLoadRoleRequest{player_uid + 1, role_id}, payload);
    response = rpc.handle(make_message(game_route::player_db_load_role(), payload));
    if (!require(response.status == yuan::rpc::RpcStatus::bad_request, "player uid mismatch should be rejected")) {
        return 6;
    }

    (void)redis->command("DEL", {"game:player_db:player_role:" + std::to_string(role_id)});
    return 0;
}
