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

    const auto now_ms = 1800000000000ULL + suffix;
    SSPlayerDbMailCreateRequest mail;
    mail.player_uid = player_uid;
    mail.role_id = role_id;
    mail.title = "Quest reward";
    mail.body = "reward attached";
    mail.sender = "system";
    mail.detail.detail_type = 1001;
    mail.detail.detail_data = yuan::rpc::Bytes{0x10, 0x20, 0x30};
    mail.attachments.push_back(SSMailAttachment{"item", 1001, 3, "bind"});
    mail.now_ms = now_ms;
    mail.dedupe_key = "quest.reward." + std::to_string(suffix);

    payload.clear();
    (void)encode_binary(mail, payload);
    response = rpc.handle(make_message(game_route::player_db_mail_create(), payload));
    auto create_mail_response = decode_binary<SSPlayerDbMailCreateResponse>(response.payload);
    if (!require(response.status == yuan::rpc::RpcStatus::ok && create_mail_response && create_mail_response->ok && create_mail_response->mail_id != 0,
                 "player mail create should return mail id")) {
        return 7;
    }
    const auto mail_id = create_mail_response->mail_id;

    payload.clear();
    (void)encode_binary(mail, payload);
    response = rpc.handle(make_message(game_route::player_db_mail_create(), payload));
    create_mail_response = decode_binary<SSPlayerDbMailCreateResponse>(response.payload);
    if (!require(response.status == yuan::rpc::RpcStatus::ok && create_mail_response && create_mail_response->duplicated && create_mail_response->mail_id == mail_id,
                 "player mail create should be idempotent by dedupe key")) {
        return 8;
    }

    SSPlayerDbMailListRequest list_mail;
    list_mail.player_uid = player_uid;
    list_mail.role_id = role_id;
    list_mail.now_ms = now_ms + 1;
    payload.clear();
    (void)encode_binary(list_mail, payload);
    response = rpc.handle(make_message(game_route::player_db_mail_list(), payload));
    auto list_mail_response = decode_binary<SSPlayerDbMailListResponse>(response.payload);
    if (!require(response.status == yuan::rpc::RpcStatus::ok && list_mail_response && list_mail_response->ok && list_mail_response->mails.size() == 1,
                 "player mail list should return personal mailbox")) {
        return 9;
    }
    if (!require(list_mail_response->mails.front().mail.detail.detail_type == 1001 && list_mail_response->mails.front().mail.detail.detail_data == yuan::rpc::Bytes{0x10, 0x20, 0x30},
                 "player mail should preserve typed detail payload")) {
        return 10;
    }

    SSPlayerDbMailGetRequest get_mail;
    get_mail.player_uid = player_uid;
    get_mail.role_id = role_id;
    get_mail.mail_id = mail_id;
    get_mail.now_ms = now_ms + 1;
    payload.clear();
    (void)encode_binary(get_mail, payload);
    response = rpc.handle(make_message(game_route::player_db_mail_get(), payload));
    auto get_mail_response = decode_binary<SSPlayerDbMailGetResponse>(response.payload);
    if (!require(response.status == yuan::rpc::RpcStatus::ok && get_mail_response && get_mail_response->has_mail && get_mail_response->mail.mail.mail_id == mail_id,
                 "player mail get should return requested mail")) {
        return 11;
    }

    SSPlayerDbMailClaimAttachmentRequest claim;
    claim.player_uid = player_uid;
    claim.role_id = role_id;
    claim.mail_id = mail_id;
    claim.now_ms = now_ms + 2;
    payload.clear();
    (void)encode_binary(claim, payload);
    response = rpc.handle(make_message(game_route::player_db_mail_claim_attachment(), payload));
    auto claim_response = decode_binary<SSPlayerDbMailClaimAttachmentResponse>(response.payload);
    if (!require(response.status == yuan::rpc::RpcStatus::ok && claim_response && claim_response->ok && claim_response->claimed && claim_response->attachments.size() == 1,
                 "first player mail attachment claim should return attachments")) {
        return 12;
    }

    payload.clear();
    (void)encode_binary(claim, payload);
    response = rpc.handle(make_message(game_route::player_db_mail_claim_attachment(), payload));
    claim_response = decode_binary<SSPlayerDbMailClaimAttachmentResponse>(response.payload);
    if (!require(response.status == yuan::rpc::RpcStatus::ok && claim_response && claim_response->ok && !claim_response->claimed && claim_response->attachments.empty(),
                 "second player mail attachment claim should be idempotent")) {
        return 13;
    }

    (void)redis->command("DEL", {"game:player_db:player_role:" + std::to_string(role_id),
                                  "game:player_db:mail:" + std::to_string(mail_id),
                                  "game:player_db:mail_index:" + std::to_string(player_uid) + ":" + std::to_string(role_id),
                                  "game:player_db:mail_state:" + std::to_string(player_uid) + ":" + std::to_string(role_id) + ":" + std::to_string(mail_id),
                                  "game:player_db:mail_dedupe:" + std::to_string(player_uid) + ":" + std::to_string(role_id) + ":" + mail.dedupe_key});
    return 0;
}
