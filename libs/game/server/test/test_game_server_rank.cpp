#include "common/game_rpc_protocol.h"
#include "common/proto/rank_proto.h"
#include "rank/rpc/rank_msg.h"

#include "option.h"
#include "redis_client.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <string>

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

    yuan::rpc::Message message(yuan::rpc::Route route, const yuan::rpc::Bytes &payload)
    {
        yuan::rpc::Message request;
        request.request_id = 42;
        request.set_continuation_id(9001);
        request.route = std::move(route);
        request.payload = payload;
        return request;
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
    option.name_ = "game-rank-test";

    auto redis = std::make_shared<yuan::redis::RedisClient>(option);
    if (!redis->ensure_connected() || !redis->ping()) {
        std::cerr << "game_server_rank skipped: local Redis 127.0.0.1:6379 unavailable\n";
        return 0;
    }

    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string board = "test:rank:" + std::to_string(suffix);
    const RoleId role_id = static_cast<RoleId>(900000000ULL + static_cast<unsigned long long>(suffix % 1000000));
    const std::string member = "role:" + std::to_string(role_id);

    RankMsgContext context;
    context.redis = redis;
    yuan::rpc::Server rpc;
    if (!require(register_rank_msg(rpc, context), "rank handlers should register")) {
        return 1;
    }

    SSRankRoleUpdateRequest role_update;
    role_update.role.role_id = role_id;
    role_update.role.player_uid = 12345;
    role_update.role.name = "Alice";
    role_update.role.level = 12;
    role_update.role.world_service_id = 7001;
    yuan::rpc::Bytes payload;
    (void)encode_binary(role_update, payload);
    auto response = rpc.handle(message(game_route::rank_role_update(), payload));
    if (!require(response.status == yuan::rpc::RpcStatus::ok, "rank role update should succeed")) {
        return 2;
    }
    auto role_update_response = decode_binary<SSRankRoleResponse>(response.payload);
    if (!require(role_update_response && role_update_response->ok && role_update_response->has_role, "rank role update response should include role")) {
        return 3;
    }

    SSRankRoleGetRequest role_get{role_id};
    payload.clear();
    (void)encode_binary(role_get, payload);
    response = rpc.handle(message(game_route::rank_role_get(), payload));
    auto role_response = decode_binary<SSRankRoleResponse>(response.payload);
    if (!require(response.status == yuan::rpc::RpcStatus::ok && role_response && role_response->has_role, "rank role get should load role")) {
        return 4;
    }
    if (!require(role_response->role.name == "Alice" && role_response->role.level == 12, "rank role fields should roundtrip")) {
        return 5;
    }

    SSRankScoreUpdateRequest score_update;
    score_update.board = board;
    score_update.member = member;
    score_update.score = 100;
    score_update.has_role = true;
    score_update.role = role_update.role;
    payload.clear();
    (void)encode_binary(score_update, payload);
    response = rpc.handle(message(game_route::rank_score_update(), payload));
    if (!require(response.status == yuan::rpc::RpcStatus::ok, "rank score update should succeed")) {
        return 6;
    }

    SSRankScoreGetRequest score_get{board, member};
    payload.clear();
    (void)encode_binary(score_get, payload);
    response = rpc.handle(message(game_route::rank_score_get(), payload));
    auto score_response = decode_binary<SSRankScoreResponse>(response.payload);
    if (!require(score_response && score_response->has_score && score_response->score == 100, "rank score get should return score")) {
        return 7;
    }
    if (!require(score_response->has_role && score_response->role.role_id == role_id, "rank score get should attach role")) {
        return 8;
    }

    SSRankScoreUpdateRequest second_score;
    second_score.board = board;
    second_score.member = "role:" + std::to_string(role_id + 1);
    second_score.score = 80;
    payload.clear();
    (void)encode_binary(second_score, payload);
    response = rpc.handle(message(game_route::rank_score_update(), payload));
    if (!require(response.status == yuan::rpc::RpcStatus::ok, "second rank score update should succeed")) {
        return 9;
    }

    SSRankTopGetRequest top_get{board, 10};
    payload.clear();
    (void)encode_binary(top_get, payload);
    response = rpc.handle(message(game_route::rank_top_get(), payload));
    auto top_response = decode_binary<SSRankTopResponse>(response.payload);
    if (!require(top_response && top_response->ok && top_response->entries.size() == 2, "rank top should return entries")) {
        return 10;
    }
    if (!require(top_response->entries[0].member == member && top_response->entries[0].score == 100 && top_response->entries[0].rank == 1,
                 "rank top should order by descending score")) {
        return 11;
    }

    SSRankScoreRemoveRequest score_remove{board, member};
    payload.clear();
    (void)encode_binary(score_remove, payload);
    response = rpc.handle(message(game_route::rank_score_remove(), payload));
    if (!require(response.status == yuan::rpc::RpcStatus::ok, "rank score remove should succeed")) {
        return 12;
    }

    payload.clear();
    (void)encode_binary(score_get, payload);
    response = rpc.handle(message(game_route::rank_score_get(), payload));
    score_response = decode_binary<SSRankScoreResponse>(response.payload);
    if (!require(score_response && !score_response->has_score, "removed rank score should be absent")) {
        return 13;
    }

    (void)redis->command("DEL", {"game:rank:" + board, "game:rank:role:" + std::to_string(role_id), "game:rank:role:" + std::to_string(role_id + 1)});
    return 0;
}
