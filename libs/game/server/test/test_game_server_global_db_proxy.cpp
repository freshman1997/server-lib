#include "common/codec/game_binary_codec.h"
#include "common/proto/global_db_proto.h"
#include "global_db_proxy/rpc/global_db_msg.h"

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
        message.request_id = 101;
        message.set_continuation_id(201);
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
    option.name_ = "game-global-db-proxy-test";

    auto redis = std::make_shared<yuan::redis::RedisClient>(option);
    if (!redis->ensure_connected() || !redis->ping()) {
        std::cerr << "game_server_global_db_proxy skipped: local Redis 127.0.0.1:6379 unavailable\n";
        return 0;
    }

    const auto suffix = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000);
    const auto key = "test.config." + std::to_string(suffix);

    GlobalDbMsgContext context;
    context.redis = redis;
    yuan::rpc::Server rpc;
    if (!require(register_global_db_msg(rpc, context), "global db handlers should register")) {
        return 1;
    }

    yuan::rpc::Bytes payload;
    (void)encode_binary(SSGlobalDbConfigGetRequest{key}, payload);
    auto response = rpc.handle(make_message(game_route::global_db_config_get(), payload));
    auto get_response = decode_binary<SSGlobalDbConfigResponse>(response.payload);
    if (!require(response.status == yuan::rpc::RpcStatus::ok && get_response && get_response->ok && !get_response->has_value,
                 "missing global config should return empty ok response")) {
        return 2;
    }

    payload.clear();
    (void)encode_binary(SSGlobalDbConfigSetRequest{key, "enabled", 7}, payload);
    response = rpc.handle(make_message(game_route::global_db_config_set(), payload));
    auto set_response = decode_binary<SSGlobalDbConfigResponse>(response.payload);
    if (!require(response.status == yuan::rpc::RpcStatus::ok && set_response && set_response->has_value && set_response->value == "enabled" && set_response->data_version == 7,
                 "global config set should save value and version")) {
        return 3;
    }

    payload.clear();
    (void)encode_binary(SSGlobalDbConfigGetRequest{key}, payload);
    response = rpc.handle(make_message(game_route::global_db_config_get(), payload));
    get_response = decode_binary<SSGlobalDbConfigResponse>(response.payload);
    if (!require(response.status == yuan::rpc::RpcStatus::ok && get_response && get_response->has_value && get_response->value == "enabled" && get_response->data_version == 7,
                 "global config should roundtrip")) {
        return 4;
    }

    payload.clear();
    (void)encode_binary(SSGlobalDbConfigGetRequest{""}, payload);
    response = rpc.handle(make_message(game_route::global_db_config_get(), payload));
    if (!require(response.status == yuan::rpc::RpcStatus::bad_request, "empty global config key should fail")) {
        return 5;
    }

    (void)redis->command("DEL", {"game:global_db:config:" + key});
    return 0;
}
