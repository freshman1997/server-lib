#include "common/codec/game_binary_codec.h"
#include "common/proto/world_db_proto.h"
#include "world_db_proxy/rpc/world_db_msg.h"

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
    option.name_ = "game-world-db-proxy-test";

    auto redis = std::make_shared<yuan::redis::RedisClient>(option);
    if (!redis->ensure_connected() || !redis->ping()) {
        std::cerr << "game_server_world_db_proxy skipped: local Redis 127.0.0.1:6379 unavailable\n";
        return 0;
    }

    const auto suffix = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000);
    const PlayerUid player_uid = 770000 + suffix;
    const RoleId role_id = 660000 + suffix;
    const PackedGameServiceId zone_service_id = pack_game_service_id(1, 1, GameServiceType::zone, 1);

    WorldDbMsgContext context;
    context.redis = redis;
    yuan::rpc::Server rpc;
    if (!require(register_world_db_msg(rpc, context), "world db handlers should register")) {
        return 1;
    }

    yuan::rpc::Bytes payload;
    (void)encode_binary(SSWorldDbRoleLocationGetRequest{role_id}, payload);
    auto response = rpc.handle(make_message(game_route::world_db_role_location_get(), payload));
    auto get_response = decode_binary<SSWorldDbRoleLocationResponse>(response.payload);
    if (!require(response.status == yuan::rpc::RpcStatus::ok && get_response && get_response->ok && !get_response->has_location,
                 "missing world role location should return empty ok response")) {
        return 2;
    }

    payload.clear();
    (void)encode_binary(SSWorldDbRoleLocationSetRequest{player_uid, role_id, zone_service_id, 123, 9}, payload);
    response = rpc.handle(make_message(game_route::world_db_role_location_set(), payload));
    auto set_response = decode_binary<SSWorldDbRoleLocationResponse>(response.payload);
    if (!require(response.status == yuan::rpc::RpcStatus::ok && set_response && set_response->has_location && set_response->data_version == 9,
                 "world role location set should save versioned location")) {
        return 3;
    }

    payload.clear();
    (void)encode_binary(SSWorldDbRoleLocationGetRequest{role_id}, payload);
    response = rpc.handle(make_message(game_route::world_db_role_location_get(), payload));
    get_response = decode_binary<SSWorldDbRoleLocationResponse>(response.payload);
    if (!require(get_response && get_response->has_location && get_response->zone_service_id == zone_service_id && get_response->gateway_session_id == 123,
                 "world role location should roundtrip")) {
        return 4;
    }

    payload.clear();
    (void)encode_binary(SSWorldDbRoleLocationSetRequest{player_uid, role_id, 0, 0, 10}, payload);
    response = rpc.handle(make_message(game_route::world_db_role_location_set(), payload));
    if (!require(response.status == yuan::rpc::RpcStatus::ok, "world role location clear should succeed")) {
        return 5;
    }

    payload.clear();
    (void)encode_binary(SSWorldDbRoleLocationGetRequest{role_id}, payload);
    response = rpc.handle(make_message(game_route::world_db_role_location_get(), payload));
    get_response = decode_binary<SSWorldDbRoleLocationResponse>(response.payload);
    if (!require(get_response && !get_response->has_location, "cleared world role location should be absent")) {
        return 6;
    }

    const RoleId ownership_role_id = role_id + 1000000;
    const PackedGameServiceId zone_b = pack_game_service_id(1, 1, GameServiceType::zone, 2);
    payload.clear();
    (void)encode_binary(SSWorldDbOwnershipCompareAndSetRequest{ownership_role_id, 0, 0, zone_service_id, 10}, payload);
    response = rpc.handle(make_message(game_route::world_db_ownership_compare_and_set(), payload));
    auto ownership_response = decode_binary<SSWorldDbOwnershipCompareAndSetResponse>(response.payload);
    if (!require(response.status == yuan::rpc::RpcStatus::ok && ownership_response && ownership_response->applied && ownership_response->has_owner,
                 "world ownership initial CAS should set owner")) {
        return 7;
    }

    payload.clear();
    (void)encode_binary(SSWorldDbOwnershipCompareAndSetRequest{ownership_role_id, zone_service_id, 10, zone_b, 20}, payload);
    response = rpc.handle(make_message(game_route::world_db_ownership_compare_and_set(), payload));
    ownership_response = decode_binary<SSWorldDbOwnershipCompareAndSetResponse>(response.payload);
    if (!require(ownership_response && ownership_response->applied && ownership_response->zone_service_id == zone_b && ownership_response->gateway_session_id == 20,
                 "world ownership newer login should replace owner")) {
        return 8;
    }

    payload.clear();
    (void)encode_binary(SSWorldDbOwnershipCompareAndSetRequest{ownership_role_id, zone_service_id, 10, 0, 0}, payload);
    response = rpc.handle(make_message(game_route::world_db_ownership_compare_and_set(), payload));
    ownership_response = decode_binary<SSWorldDbOwnershipCompareAndSetResponse>(response.payload);
    if (!require(ownership_response && !ownership_response->applied && ownership_response->has_owner && ownership_response->zone_service_id == zone_b,
                 "world ownership stale logout should be rejected")) {
        return 9;
    }

    payload.clear();
    (void)encode_binary(SSWorldDbOwnershipCompareAndSetRequest{ownership_role_id, zone_b, 20, 0, 0}, payload);
    response = rpc.handle(make_message(game_route::world_db_ownership_compare_and_set(), payload));
    ownership_response = decode_binary<SSWorldDbOwnershipCompareAndSetResponse>(response.payload);
    if (!require(ownership_response && ownership_response->applied && !ownership_response->has_owner,
                 "world ownership current logout should clear owner")) {
        return 10;
    }

    (void)redis->command("DEL", {"game:world_db:role_location:" + std::to_string(role_id)});
    (void)redis->command("DEL", {"game:world_db:ownership:" + std::to_string(ownership_role_id)});
    return 0;
}
