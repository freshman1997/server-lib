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
    const auto player_uid = 900000 + suffix;
    const auto role_id = 910000 + suffix;
    const auto now_ms = 1800000000000ULL + suffix;

    SSGlobalDbMailCreateRequest player_mail;
    player_mail.scope = global_db_mail_scope::player;
    player_mail.player_uid = player_uid;
    player_mail.role_id = role_id;
    player_mail.title = "wrong proxy";
    payload.clear();
    (void)encode_binary(player_mail, payload);
    response = rpc.handle(make_message(game_route::global_db_mail_create(), payload));
    if (!require(response.status == yuan::rpc::RpcStatus::bad_request, "global db should reject personal player mail")) {
        return 6;
    }

    SSGlobalDbMailCreateRequest global_mail;
    global_mail.scope = global_db_mail_scope::global;
    global_mail.title = "Global compensation";
    global_mail.body = "all players";
    global_mail.sender = "system";
    global_mail.detail.detail_type = 2001;
    global_mail.detail.detail_data = yuan::rpc::Bytes{0x44, 0x55};
    global_mail.now_ms = now_ms + 1;
    global_mail.dedupe_key = "global.compensation." + std::to_string(suffix);
    payload.clear();
    (void)encode_binary(global_mail, payload);
    response = rpc.handle(make_message(game_route::global_db_mail_create(), payload));
    auto create_mail_response = decode_binary<SSGlobalDbMailCreateResponse>(response.payload);
    if (!require(response.status == yuan::rpc::RpcStatus::ok && create_mail_response && create_mail_response->ok,
                 "global mail create should succeed")) {
        return 7;
    }
    const auto global_mail_id = create_mail_response->mail_id;

    payload.clear();
    (void)encode_binary(global_mail, payload);
    response = rpc.handle(make_message(game_route::global_db_mail_create(), payload));
    create_mail_response = decode_binary<SSGlobalDbMailCreateResponse>(response.payload);
    if (!require(response.status == yuan::rpc::RpcStatus::ok && create_mail_response && create_mail_response->duplicated && create_mail_response->mail_id == global_mail_id,
                 "global mail create should be idempotent by dedupe key")) {
        return 8;
    }

    SSGlobalDbMailCreateRequest operator_mail;
    operator_mail.scope = global_db_mail_scope::operator_;
    operator_mail.title = "Maintenance notice";
    operator_mail.body = "operator message";
    operator_mail.sender = "ops";
    operator_mail.operator_id = "gm-1";
    operator_mail.operator_reason = "maintenance";
    operator_mail.now_ms = now_ms + 2;
    payload.clear();
    (void)encode_binary(operator_mail, payload);
    response = rpc.handle(make_message(game_route::global_db_mail_create(), payload));
    create_mail_response = decode_binary<SSGlobalDbMailCreateResponse>(response.payload);
    if (!require(response.status == yuan::rpc::RpcStatus::ok && create_mail_response && create_mail_response->ok,
                 "operator mail without detail should succeed")) {
        return 9;
    }
    const auto operator_mail_id = create_mail_response->mail_id;

    SSGlobalDbMailListRequest list_mail;
    list_mail.player_uid = player_uid;
    list_mail.role_id = role_id;
    list_mail.now_ms = now_ms + 3;
    payload.clear();
    (void)encode_binary(list_mail, payload);
    response = rpc.handle(make_message(game_route::global_db_mail_list(), payload));
    auto list_mail_response = decode_binary<SSGlobalDbMailListResponse>(response.payload);
    if (!require(response.status == yuan::rpc::RpcStatus::ok && list_mail_response && list_mail_response->ok && list_mail_response->mails.size() == 2,
                 "mail list should include global/operator mails for periodic polling")) {
        return 10;
    }

    bool found_global_detail = false;
    bool found_operator_without_detail = false;
    for (const auto &mail : list_mail_response->mails) {
        if (mail.mail.mail_id == global_mail_id) {
            found_global_detail = mail.mail.detail.detail_type == 2001 && mail.mail.detail.detail_data == yuan::rpc::Bytes{0x44, 0x55};
        }
        if (mail.mail.mail_id == operator_mail_id) {
            found_operator_without_detail = mail.mail.scope == global_db_mail_scope::operator_ && mail.mail.detail.detail_type == 0 && mail.mail.detail.detail_data.empty() && mail.mail.operator_id == "gm-1";
        }
    }
    if (!require(found_global_detail && found_operator_without_detail, "global polling should preserve detail wrapper and operator metadata")) {
        return 11;
    }

    SSGlobalDbMailGetRequest get_mail;
    get_mail.player_uid = player_uid;
    get_mail.role_id = role_id;
    get_mail.mail_id = global_mail_id;
    get_mail.now_ms = now_ms + 3;
    payload.clear();
    (void)encode_binary(get_mail, payload);
    response = rpc.handle(make_message(game_route::global_db_mail_get(), payload));
    auto get_mail_response = decode_binary<SSGlobalDbMailGetResponse>(response.payload);
    if (!require(response.status == yuan::rpc::RpcStatus::ok && get_mail_response && get_mail_response->has_mail && get_mail_response->mail.mail.detail.detail_type == 2001,
                 "global mail get should preserve typed detail")) {
        return 12;
    }

    (void)redis->command("DEL", {"game:global_db:mail:" + std::to_string(global_mail_id),
                                  "game:global_db:mail:" + std::to_string(operator_mail_id),
                                  "game:global_db:mail_index:global",
                                  "game:global_db:mail_index:operator",
                                  "game:global_db:mail_dedupe:" + std::to_string(global_db_mail_scope::global) + ":0:0:" + global_mail.dedupe_key});
    return 0;
}
