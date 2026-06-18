#include "global_db_proxy/rpc/global_db_msg.h"

#include "common/metadata_keys.h"
#include "common/proto/global_db_proto.h"
#include "common/storage/entity_store.h"
#include "common/storage/redis_orm_store.h"

#include <functional>
#include <algorithm>
#include <chrono>

namespace yuan::game::server
{
    namespace
    {
        constexpr const char *kConfigTable = "config";
        constexpr const char *kMailTable = "mail";
        constexpr const char *kMailIndexTable = "mail_index";
        constexpr const char *kMailStateTable = "mail_state";
        constexpr const char *kMailDedupeTable = "mail_dedupe";

        std::uint64_t now_ms()
        {
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        }

        bool valid_config_key(const std::string &key)
        {
            return !key.empty() && key.size() <= 128;
        }

        bool valid_mail_scope(std::uint32_t scope)
        {
            return scope == global_db_mail_scope::player || scope == global_db_mail_scope::global || scope == global_db_mail_scope::operator_;
        }

        bool visible_at(const SSGlobalDbMailRecord &mail, std::uint64_t current_ms)
        {
            return (mail.starts_at_ms == 0 || mail.starts_at_ms <= current_ms) && (mail.expires_at_ms == 0 || mail.expires_at_ms > current_ms);
        }

        std::string mail_key(std::uint64_t mail_id)
        {
            return std::to_string(mail_id);
        }

        std::string player_index_key(PlayerUid player_uid)
        {
            return "player:" + std::to_string(player_uid);
        }

        std::string global_index_key(std::uint32_t scope)
        {
            return scope == global_db_mail_scope::operator_ ? "operator" : "global";
        }

        std::string mail_state_key(PlayerUid player_uid, RoleId role_id, std::uint64_t mail_id)
        {
            return std::to_string(player_uid) + ":" + std::to_string(role_id) + ":" + std::to_string(mail_id);
        }

        std::string dedupe_key(std::uint32_t scope, PlayerUid player_uid, RoleId role_id, const std::string &dedupe)
        {
            return std::to_string(scope) + ":" + std::to_string(player_uid) + ":" + std::to_string(role_id) + ":" + dedupe;
        }

        storage::EntityRecord config_record(const SSGlobalDbConfigSetRequest &request, std::uint64_t version)
        {
            return storage::EntityRecord{kConfigTable,
                                         request.key,
                                         {{"key", request.key}, {"value", request.value}},
                                         version};
        }

        SSGlobalDbConfigResponse decode_config_record(const storage::EntityRecord &record)
        {
            SSGlobalDbConfigResponse response;
            response.ok = true;
            response.message = "ok";
            response.has_value = true;
            response.key = record.fields.contains("key") ? record.fields.at("key") : record.key;
            response.value = record.fields.contains("value") ? record.fields.at("value") : std::string{};
            response.data_version = record.version;
            return response;
        }

        yuan::rpc::Bytes encode_blob(const auto &value)
        {
            yuan::rpc::Bytes blob;
            (void)encode_binary(value, blob);
            return blob;
        }

        storage::EntityRecord mail_record(const SSGlobalDbMailRecord &mail)
        {
            return storage::EntityRecord{kMailTable,
                                         mail_key(mail.mail_id),
                                         {{"mail_id", std::to_string(mail.mail_id)},
                                          {"scope", std::to_string(mail.scope)},
                                          {"player_uid", std::to_string(mail.player_uid)},
                                          {"role_id", std::to_string(mail.role_id)},
                                          {"title", mail.title},
                                          {"created_at_ms", std::to_string(mail.created_at_ms)},
                                          {"expires_at_ms", std::to_string(mail.expires_at_ms)}},
                                         mail.data_version,
                                         encode_blob(mail)};
        }

        std::optional<SSGlobalDbMailRecord> decode_mail_record(const storage::EntityRecord &record)
        {
            if (record.object_blob.empty()) {
                return std::nullopt;
            }
            return decode_binary<SSGlobalDbMailRecord>(record.object_blob);
        }

        SSMailIdList load_index(storage::EntityStore &entities, const std::string &key)
        {
            if (auto record = entities.load(kMailIndexTable, key)) {
                if (auto decoded = decode_binary<SSMailIdList>(record->object_blob)) {
                    return *decoded;
                }
            }
            return {};
        }

        bool save_index(storage::EntityStore &entities, const std::string &key, SSMailIdList index)
        {
            auto ids = index.mail_ids;
            std::sort(ids.begin(), ids.end());
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
            index.mail_ids = std::move(ids);
            const auto saved = entities.save(storage::EntityRecord{kMailIndexTable, key, {{"key", key}}, 1, encode_blob(index)});
            return saved.ok;
        }

        SSMailStateRecord load_mail_state(storage::EntityStore &entities, PlayerUid player_uid, RoleId role_id, std::uint64_t mail_id)
        {
            if (auto record = entities.load(kMailStateTable, mail_state_key(player_uid, role_id, mail_id))) {
                if (auto decoded = decode_binary<SSMailStateRecord>(record->object_blob)) {
                    return *decoded;
                }
            }
            return {};
        }

        bool save_mail_state(storage::EntityStore &entities, PlayerUid player_uid, RoleId role_id, std::uint64_t mail_id, SSMailStateRecord state)
        {
            const auto key = mail_state_key(player_uid, role_id, mail_id);
            const auto saved = entities.save(storage::EntityRecord{kMailStateTable,
                                                                    key,
                                                                    {{"player_uid", std::to_string(player_uid)},
                                                                     {"role_id", std::to_string(role_id)},
                                                                     {"mail_id", std::to_string(mail_id)},
                                                                     {"state", std::to_string(state.state)}},
                                                                    1,
                                                                    encode_blob(state)});
            return saved.ok;
        }

        SSGlobalDbMailBoxItem mail_box_item(storage::EntityStore &entities, const SSGlobalDbMailRecord &mail, PlayerUid player_uid, RoleId role_id)
        {
            SSGlobalDbMailBoxItem item;
            item.mail = mail;
            const auto state = load_mail_state(entities, player_uid, role_id, mail.mail_id);
            item.state = state.state;
            item.attachment_claimed = state.attachment_claimed;
            item.claimed_at_ms = state.claimed_at_ms;
            return item;
        }

        bool ensure_redis(GlobalDbMsgContext &context)
        {
            return context.redis_pool || (context.redis && context.redis->ensure_connected());
        }

        storage::RedisOrmStore make_store(GlobalDbMsgContext &context)
        {
            if (context.redis_pool) {
                return storage::RedisOrmStore(context.redis_pool, "game:global_db:");
            }
            return storage::RedisOrmStore(context.redis, "game:global_db:");
        }

        storage::RedisOrmStore make_store(yuan::redis::RedisClient &redis)
        {
            return storage::RedisOrmStore(std::shared_ptr<yuan::redis::RedisClient>(&redis, [](yuan::redis::RedisClient *) {}), "game:global_db:");
        }

        std::uint64_t message_connection_id(const yuan::rpc::Message &message)
        {
            const auto it = message.metadata.find(game_metadata_key::rpc_connection_id);
            if (it == message.metadata.end()) {
                return 0;
            }
            try {
                return static_cast<std::uint64_t>(std::stoull(it->second));
            } catch (...) {
                return 0;
            }
        }

        yuan::rpc::Response response_for(const yuan::rpc::Message &message, yuan::rpc::RpcStatus status, yuan::rpc::Bytes payload = {}, std::string error = {})
        {
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            response.status = status;
            response.payload = std::move(payload);
            response.error = std::move(error);
            return response;
        }

        yuan::rpc::Response deferred_response_for(const yuan::rpc::Message &message)
        {
            auto response = response_for(message, yuan::rpc::RpcStatus::ok);
            response.metadata[game_metadata_key::rpc_defer_response] = "1";
            return response;
        }

        yuan::rpc::Response config_get_response(const yuan::rpc::Message &message,
                                                const SSGlobalDbConfigGetRequest &request,
                                                storage::RedisOrmStore orm)
        {
            SSGlobalDbConfigResponse payload_response;
            payload_response.ok = true;
            payload_response.message = "ok";
            payload_response.key = request.key;
            storage::EntityStore entities(orm);
            if (auto record = entities.load(kConfigTable, request.key)) {
                payload_response = decode_config_record(*record);
            }
            yuan::rpc::Bytes payload;
            (void)encode_binary(payload_response, payload);
            return response_for(message, yuan::rpc::RpcStatus::ok, std::move(payload));
        }

        yuan::rpc::Response config_set_response(const yuan::rpc::Message &message,
                                                const SSGlobalDbConfigSetRequest &request,
                                                storage::RedisOrmStore orm)
        {
            storage::EntityStore entities(orm);
            const auto next_version = request.data_version == 0 ? 1 : request.data_version;
            const auto saved = entities.save(config_record(request, next_version));
            if (!saved.ok) {
                return response_for(message, yuan::rpc::RpcStatus::unavailable, {}, "failed to save global config");
            }
            SSGlobalDbConfigResponse payload_response{true, "ok", true, request.key, request.value, next_version};
            yuan::rpc::Bytes payload;
            (void)encode_binary(payload_response, payload);
            return response_for(message, yuan::rpc::RpcStatus::ok, std::move(payload));
        }

        yuan::rpc::Response mail_create_response(const yuan::rpc::Message &message,
                                                 const SSGlobalDbMailCreateRequest &request,
                                                 storage::RedisOrmStore orm)
        {
            if (!valid_mail_scope(request.scope) || request.title.empty()) {
                return response_for(message, yuan::rpc::RpcStatus::bad_request, {}, "invalid mail create request");
            }
            if (request.scope == global_db_mail_scope::player) {
                return response_for(message, yuan::rpc::RpcStatus::bad_request, {}, "player mail belongs to player_db_proxy");
            }

            storage::EntityStore entities(orm);
            if (!request.dedupe_key.empty()) {
                if (auto dedupe = entities.load(kMailDedupeTable, dedupe_key(request.scope, request.player_uid, request.role_id, request.dedupe_key))) {
                    SSGlobalDbMailCreateResponse payload_response{true, "ok", storage::field_u64(dedupe->fields, "mail_id", 0), true, dedupe->version};
                    yuan::rpc::Bytes payload;
                    (void)encode_binary(payload_response, payload);
                    return response_for(message, yuan::rpc::RpcStatus::ok, std::move(payload));
                }
            }

            const auto created_ms = request.now_ms == 0 ? now_ms() : request.now_ms;
            const auto mail_id = created_ms * 1000 + static_cast<std::uint64_t>(message.request_id % 1000);
            SSGlobalDbMailRecord mail;
            mail.mail_id = mail_id;
            mail.scope = request.scope;
            mail.player_uid = request.player_uid;
            mail.role_id = request.role_id;
            mail.title = request.title;
            mail.body = request.body;
            mail.detail = request.detail;
            mail.sender = request.sender;
            mail.operator_id = request.operator_id;
            mail.operator_reason = request.operator_reason;
            mail.attachments = request.attachments;
            mail.created_at_ms = created_ms;
            mail.starts_at_ms = request.starts_at_ms;
            mail.expires_at_ms = request.expires_at_ms;
            mail.dedupe_key = request.dedupe_key;
            mail.data_version = 1;

            auto saved = entities.insert(mail_record(mail));
            if (!saved.ok) {
                return response_for(message, yuan::rpc::RpcStatus::unavailable, {}, "failed to save mail");
            }

            const auto index_key = request.scope == global_db_mail_scope::player ? player_index_key(request.player_uid) : global_index_key(request.scope);
            auto index = load_index(entities, index_key);
            index.mail_ids.push_back(mail_id);
            if (!save_index(entities, index_key, std::move(index))) {
                return response_for(message, yuan::rpc::RpcStatus::unavailable, {}, "failed to save mail index");
            }

            if (!request.dedupe_key.empty()) {
                const auto key = dedupe_key(request.scope, request.player_uid, request.role_id, request.dedupe_key);
                (void)entities.insert(storage::EntityRecord{kMailDedupeTable, key, {{"mail_id", std::to_string(mail_id)}}, 1});
            }

            SSGlobalDbMailCreateResponse payload_response{true, "ok", mail_id, false, 1};
            yuan::rpc::Bytes payload;
            (void)encode_binary(payload_response, payload);
            return response_for(message, yuan::rpc::RpcStatus::ok, std::move(payload));
        }

        yuan::rpc::Response mail_list_response(const yuan::rpc::Message &message,
                                               const SSGlobalDbMailListRequest &request,
                                               storage::RedisOrmStore orm)
        {
            if (request.player_uid == 0) {
                return response_for(message, yuan::rpc::RpcStatus::bad_request, {}, "player_uid is required");
            }
            storage::EntityStore entities(orm);
            std::vector<std::uint64_t> ids;
            auto append_ids = [&ids](SSMailIdList list) {
                ids.insert(ids.end(), list.mail_ids.begin(), list.mail_ids.end());
            };
            append_ids(load_index(entities, player_index_key(request.player_uid)));
            if (request.include_global) {
                append_ids(load_index(entities, global_index_key(global_db_mail_scope::global)));
            }
            if (request.include_operator) {
                append_ids(load_index(entities, global_index_key(global_db_mail_scope::operator_)));
            }
            std::sort(ids.begin(), ids.end(), std::greater<>());
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

            SSGlobalDbMailListResponse payload_response;
            payload_response.ok = true;
            payload_response.message = "ok";
            const auto current_ms = request.now_ms == 0 ? now_ms() : request.now_ms;
            const auto limit = std::clamp(request.limit, static_cast<std::uint32_t>(1), static_cast<std::uint32_t>(200));
            for (const auto id : ids) {
                if (payload_response.mails.size() >= limit) {
                    break;
                }
                auto record = entities.load(kMailTable, mail_key(id));
                if (!record) {
                    continue;
                }
                auto mail = decode_mail_record(*record);
                if (!mail || !visible_at(*mail, current_ms)) {
                    continue;
                }
                payload_response.mails.push_back(mail_box_item(entities, *mail, request.player_uid, request.role_id));
            }
            yuan::rpc::Bytes payload;
            (void)encode_binary(payload_response, payload);
            return response_for(message, yuan::rpc::RpcStatus::ok, std::move(payload));
        }

        yuan::rpc::Response mail_get_response(const yuan::rpc::Message &message,
                                              const SSGlobalDbMailGetRequest &request,
                                              storage::RedisOrmStore orm)
        {
            if (request.player_uid == 0 || request.mail_id == 0) {
                return response_for(message, yuan::rpc::RpcStatus::bad_request, {}, "player_uid and mail_id are required");
            }
            storage::EntityStore entities(orm);
            SSGlobalDbMailGetResponse payload_response;
            payload_response.ok = true;
            payload_response.message = "ok";
            const auto record = entities.load(kMailTable, mail_key(request.mail_id));
            const auto current_ms = request.now_ms == 0 ? now_ms() : request.now_ms;
            if (record) {
                if (auto mail = decode_mail_record(*record); mail && visible_at(*mail, current_ms)) {
                    payload_response.has_mail = true;
                    payload_response.mail = mail_box_item(entities, *mail, request.player_uid, request.role_id);
                }
            }
            yuan::rpc::Bytes payload;
            (void)encode_binary(payload_response, payload);
            return response_for(message, yuan::rpc::RpcStatus::ok, std::move(payload));
        }

        yuan::rpc::Response mail_claim_attachment_response(const yuan::rpc::Message &message,
                                                           const SSGlobalDbMailClaimAttachmentRequest &request,
                                                           storage::RedisOrmStore orm)
        {
            if (request.player_uid == 0 || request.mail_id == 0) {
                return response_for(message, yuan::rpc::RpcStatus::bad_request, {}, "player_uid and mail_id are required");
            }
            storage::EntityStore entities(orm);
            const auto record = entities.load(kMailTable, mail_key(request.mail_id));
            const auto current_ms = request.now_ms == 0 ? now_ms() : request.now_ms;
            if (!record) {
                SSGlobalDbMailClaimAttachmentResponse payload_response{false, "mail not found", false, request.mail_id, {}, 0};
                yuan::rpc::Bytes payload;
                (void)encode_binary(payload_response, payload);
                return response_for(message, yuan::rpc::RpcStatus::ok, std::move(payload));
            }
            auto mail = decode_mail_record(*record);
            if (!mail || !visible_at(*mail, current_ms)) {
                SSGlobalDbMailClaimAttachmentResponse payload_response{false, "mail not available", false, request.mail_id, {}, 0};
                yuan::rpc::Bytes payload;
                (void)encode_binary(payload_response, payload);
                return response_for(message, yuan::rpc::RpcStatus::ok, std::move(payload));
            }
            if (mail->attachments.empty()) {
                SSGlobalDbMailClaimAttachmentResponse payload_response{false, "mail has no attachments", false, request.mail_id, {}, 0};
                yuan::rpc::Bytes payload;
                (void)encode_binary(payload_response, payload);
                return response_for(message, yuan::rpc::RpcStatus::ok, std::move(payload));
            }
            auto state = load_mail_state(entities, request.player_uid, request.role_id, request.mail_id);
            if (state.attachment_claimed) {
                SSGlobalDbMailClaimAttachmentResponse payload_response{true, "already claimed", false, request.mail_id, {}, state.claimed_at_ms};
                yuan::rpc::Bytes payload;
                (void)encode_binary(payload_response, payload);
                return response_for(message, yuan::rpc::RpcStatus::ok, std::move(payload));
            }
            state.attachment_claimed = true;
            state.state = mail_state::attachment_claimed;
            state.claimed_at_ms = current_ms;
            if (!save_mail_state(entities, request.player_uid, request.role_id, request.mail_id, state)) {
                return response_for(message, yuan::rpc::RpcStatus::unavailable, {}, "failed to save mail state");
            }
            SSGlobalDbMailClaimAttachmentResponse payload_response{true, "ok", true, request.mail_id, mail->attachments, state.claimed_at_ms};
            yuan::rpc::Bytes payload;
            (void)encode_binary(payload_response, payload);
            return response_for(message, yuan::rpc::RpcStatus::ok, std::move(payload));
        }

        template <typename Fn>
        bool submit_deferred(GlobalDbMsgContext &context, const yuan::rpc::Message &message, Fn &&fn)
        {
            const auto connection_id = message_connection_id(message);
            if (!context.redis_executor || !context.write_deferred_response || connection_id == 0) {
                return false;
            }
            const auto request_id = message.request_id;
            const auto continuation_id = message.continuation_id();
            context.redis_executor->submit_callback(context.resume_runtime,
                                                    std::forward<Fn>(fn),
                                                    [connection_id, request_id, continuation_id, write = context.write_deferred_response](std::optional<yuan::rpc::Response> response, std::exception_ptr exception) mutable {
                                                        if (exception) {
                                                            yuan::rpc::Response error;
                                                            error.request_id = request_id;
                                                            error.set_continuation_id(continuation_id);
                                                            error.status = yuan::rpc::RpcStatus::unavailable;
                                                            error.error = "redis unavailable";
                                                            write(connection_id, std::move(error));
                                                            return;
                                                        }
                                                        write(connection_id, std::move(*response));
                                                    });
            return true;
        }

        yuan::rpc::Response handle_global_db_config_get(GlobalDbMsgContext &context, const yuan::rpc::Message &message)
        {
            const auto request = decode_binary<SSGlobalDbConfigGetRequest>(message.payload);
            if (!request || !valid_config_key(request->key)) {
                return response_for(message, yuan::rpc::RpcStatus::bad_request, {}, "invalid global config get request");
            }
            if (!ensure_redis(context)) {
                return response_for(message, yuan::rpc::RpcStatus::unavailable, {}, "redis unavailable");
            }
            if (submit_deferred(context, message, [message, request = *request](yuan::redis::RedisClient &redis) {
                    return config_get_response(message, request, make_store(redis));
                })) {
                return deferred_response_for(message);
            }
            return config_get_response(message, *request, make_store(context));
        }

        yuan::rpc::Response handle_global_db_config_set(GlobalDbMsgContext &context, const yuan::rpc::Message &message)
        {
            const auto request = decode_binary<SSGlobalDbConfigSetRequest>(message.payload);
            if (!request || !valid_config_key(request->key)) {
                return response_for(message, yuan::rpc::RpcStatus::bad_request, {}, "invalid global config set request");
            }
            if (!ensure_redis(context)) {
                return response_for(message, yuan::rpc::RpcStatus::unavailable, {}, "redis unavailable");
            }
            if (submit_deferred(context, message, [message, request = *request](yuan::redis::RedisClient &redis) {
                    return config_set_response(message, request, make_store(redis));
                })) {
                return deferred_response_for(message);
            }
            return config_set_response(message, *request, make_store(context));
        }

        yuan::rpc::Response handle_global_db_mail_create(GlobalDbMsgContext &context, const yuan::rpc::Message &message)
        {
            const auto request = decode_binary<SSGlobalDbMailCreateRequest>(message.payload);
            if (!request) {
                return response_for(message, yuan::rpc::RpcStatus::bad_request, {}, "invalid mail create payload");
            }
            if (!ensure_redis(context)) {
                return response_for(message, yuan::rpc::RpcStatus::unavailable, {}, "redis unavailable");
            }
            if (submit_deferred(context, message, [message, request = *request](yuan::redis::RedisClient &redis) {
                    return mail_create_response(message, request, make_store(redis));
                })) {
                return deferred_response_for(message);
            }
            return mail_create_response(message, *request, make_store(context));
        }

        yuan::rpc::Response handle_global_db_mail_list(GlobalDbMsgContext &context, const yuan::rpc::Message &message)
        {
            const auto request = decode_binary<SSGlobalDbMailListRequest>(message.payload);
            if (!request) {
                return response_for(message, yuan::rpc::RpcStatus::bad_request, {}, "invalid mail list payload");
            }
            if (!ensure_redis(context)) {
                return response_for(message, yuan::rpc::RpcStatus::unavailable, {}, "redis unavailable");
            }
            if (submit_deferred(context, message, [message, request = *request](yuan::redis::RedisClient &redis) {
                    return mail_list_response(message, request, make_store(redis));
                })) {
                return deferred_response_for(message);
            }
            return mail_list_response(message, *request, make_store(context));
        }

        yuan::rpc::Response handle_global_db_mail_get(GlobalDbMsgContext &context, const yuan::rpc::Message &message)
        {
            const auto request = decode_binary<SSGlobalDbMailGetRequest>(message.payload);
            if (!request) {
                return response_for(message, yuan::rpc::RpcStatus::bad_request, {}, "invalid mail get payload");
            }
            if (!ensure_redis(context)) {
                return response_for(message, yuan::rpc::RpcStatus::unavailable, {}, "redis unavailable");
            }
            if (submit_deferred(context, message, [message, request = *request](yuan::redis::RedisClient &redis) {
                    return mail_get_response(message, request, make_store(redis));
                })) {
                return deferred_response_for(message);
            }
            return mail_get_response(message, *request, make_store(context));
        }

        yuan::rpc::Response handle_global_db_mail_claim_attachment(GlobalDbMsgContext &context, const yuan::rpc::Message &message)
        {
            const auto request = decode_binary<SSGlobalDbMailClaimAttachmentRequest>(message.payload);
            if (!request) {
                return response_for(message, yuan::rpc::RpcStatus::bad_request, {}, "invalid mail claim payload");
            }
            if (!ensure_redis(context)) {
                return response_for(message, yuan::rpc::RpcStatus::unavailable, {}, "redis unavailable");
            }
            if (submit_deferred(context, message, [message, request = *request](yuan::redis::RedisClient &redis) {
                    return mail_claim_attachment_response(message, request, make_store(redis));
                })) {
                return deferred_response_for(message);
            }
            return mail_claim_attachment_response(message, *request, make_store(context));
        }
    }

    bool register_global_db_msg(yuan::rpc::Server &server, GlobalDbMsgContext &context)
    {
        const bool get_config = server.register_handler(game_route::global_db_config_get(), std::bind_front(handle_global_db_config_get, std::ref(context)));
        const bool set_config = server.register_handler(game_route::global_db_config_set(), std::bind_front(handle_global_db_config_set, std::ref(context)));
        const bool create_mail = server.register_handler(game_route::global_db_mail_create(), std::bind_front(handle_global_db_mail_create, std::ref(context)));
        const bool list_mail = server.register_handler(game_route::global_db_mail_list(), std::bind_front(handle_global_db_mail_list, std::ref(context)));
        const bool get_mail = server.register_handler(game_route::global_db_mail_get(), std::bind_front(handle_global_db_mail_get, std::ref(context)));
        const bool claim_mail = server.register_handler(game_route::global_db_mail_claim_attachment(), std::bind_front(handle_global_db_mail_claim_attachment, std::ref(context)));

        return get_config && set_config && create_mail && list_mail && get_mail && claim_mail;
    }
}
