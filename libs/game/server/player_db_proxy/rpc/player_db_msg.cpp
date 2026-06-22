#include "player_db_proxy/rpc/player_db_msg.h"

#include "common/metadata_keys.h"
#include "common/storage/entity_store.h"
#include "common/storage/redis_orm_store.h"

#include <algorithm>
#include <chrono>
#include <functional>

namespace yuan::game::server
{
    namespace
    {
        constexpr const char *kRoleTable = "player_role";
        constexpr const char *kMailTable = "mail";
        constexpr const char *kMailIndexTable = "mail_index";
        constexpr const char *kMailStateTable = "mail_state";
        constexpr const char *kMailDedupeTable = "mail_dedupe";

        std::uint64_t now_ms()
        {
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        }

        std::string role_key(RoleId role_id)
        {
            return std::to_string(role_id);
        }

        std::string mail_key(std::uint64_t mail_id)
        {
            return std::to_string(mail_id);
        }

        std::string mail_index_key(PlayerUid player_uid, RoleId role_id)
        {
            return std::to_string(player_uid) + ":" + std::to_string(role_id);
        }

        std::string mail_state_key(PlayerUid player_uid, RoleId role_id, std::uint64_t mail_id)
        {
            return std::to_string(player_uid) + ":" + std::to_string(role_id) + ":" + std::to_string(mail_id);
        }

        std::string mail_dedupe_key(PlayerUid player_uid, RoleId role_id, const std::string &dedupe_key)
        {
            return std::to_string(player_uid) + ":" + std::to_string(role_id) + ":" + dedupe_key;
        }

        bool mail_visible_at(const SSPlayerDbMailRecord &mail, std::uint64_t current_ms)
        {
            return (mail.starts_at_ms == 0 || mail.starts_at_ms <= current_ms) && (mail.expires_at_ms == 0 || mail.expires_at_ms > current_ms);
        }

        yuan::rpc::Bytes encode_blob(const auto &value)
        {
            yuan::rpc::Bytes blob;
            (void)encode_binary(value, blob);
            return blob;
        }

        storage::EntityRecord role_record(const SSPlayerRoleData &role, std::uint64_t version)
        {
            storage::EntityRecord record{kRoleTable,
                                         role_key(role.role_id),
                                         {{"player_uid", std::to_string(role.player_uid)},
                                          {"role_id", std::to_string(role.role_id)},
                                          {"level", std::to_string(role.level)},
                                          {"exp", std::to_string(role.exp)}},
                                         version};
            (void)encode_binary(role, record.object_blob);
            return record;
        }

        storage::EntityRecord mail_record(const SSPlayerDbMailRecord &mail)
        {
            return storage::EntityRecord{kMailTable,
                                         mail_key(mail.mail_id),
                                         {{"mail_id", std::to_string(mail.mail_id)},
                                          {"player_uid", std::to_string(mail.player_uid)},
                                          {"role_id", std::to_string(mail.role_id)},
                                          {"title", mail.title},
                                          {"created_at_ms", std::to_string(mail.created_at_ms)},
                                          {"expires_at_ms", std::to_string(mail.expires_at_ms)}},
                                         mail.data_version,
                                         encode_blob(mail)};
        }

        std::optional<SSPlayerDbMailRecord> decode_mail_record(const storage::EntityRecord &record)
        {
            if (record.object_blob.empty()) {
                return std::nullopt;
            }
            return decode_binary<SSPlayerDbMailRecord>(record.object_blob);
        }

        SSMailIdList load_mail_index(storage::EntityStore &entities, PlayerUid player_uid, RoleId role_id)
        {
            if (auto record = entities.load(kMailIndexTable, mail_index_key(player_uid, role_id))) {
                if (auto decoded = decode_binary<SSMailIdList>(record->object_blob)) {
                    return *decoded;
                }
            }
            return {};
        }

        bool save_mail_index(storage::EntityStore &entities, PlayerUid player_uid, RoleId role_id, SSMailIdList index)
        {
            auto ids = index.mail_ids;
            std::sort(ids.begin(), ids.end());
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
            index.mail_ids = std::move(ids);
            const auto key = mail_index_key(player_uid, role_id);
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

        SSPlayerDbMailBoxItem mail_box_item(storage::EntityStore &entities, const SSPlayerDbMailRecord &mail)
        {
            SSPlayerDbMailBoxItem item;
            item.mail = mail;
            const auto state = load_mail_state(entities, mail.player_uid, mail.role_id, mail.mail_id);
            item.state = state.state;
            item.attachment_claimed = state.attachment_claimed;
            item.claimed_at_ms = state.claimed_at_ms;
            return item;
        }

        std::optional<SSPlayerDbRoleResponse> decode_role_record(const storage::EntityRecord &record)
        {
            SSPlayerDbRoleResponse response;
            response.ok = true;
            response.message = "ok";
            response.has_role = true;
            if (!record.object_blob.empty()) {
                if (auto role = decode_binary<SSPlayerRoleData>(record.object_blob)) {
                    response.role = *role;
                }
            }

            if (response.role.role_id == 0) {
                response.role.player_uid = storage::field_u64(record.fields, "player_uid", 0);
                response.role.role_id = storage::field_u64(record.fields, "role_id", 0);
                response.role.level = storage::field_u32(record.fields, "level", 1);
                response.role.exp = storage::field_u64(record.fields, "exp", 0);
            }

            response.data_version = record.version;
            if (response.role.player_uid == 0 || response.role.role_id == 0) {
                return std::nullopt;
            }

            return response;
        }

        bool ensure_redis(PlayerDbMsgContext &context)
        {
            return context.redis_pool || (context.redis && context.redis->ensure_connected());
        }

        storage::RedisOrmStore make_store(PlayerDbMsgContext &context)
        {
            if (context.redis_pool) {
                return storage::RedisOrmStore(context.redis_pool, "game:player_db:");
            }
            return storage::RedisOrmStore(context.redis, "game:player_db:");
        }

        storage::RedisOrmStore make_store(yuan::redis::RedisClient &redis)
        {
            return storage::RedisOrmStore(std::shared_ptr<yuan::redis::RedisClient>(&redis, [](yuan::redis::RedisClient *) {}), "game:player_db:");
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

        yuan::rpc::Response load_role_response(const yuan::rpc::Message &message,
                                               const SSPlayerDbLoadRoleRequest &request,
                                               storage::RedisOrmStore orm)
        {
            SSPlayerDbRoleResponse payload_response;
            payload_response.ok = true;
            payload_response.message = "ok";

            storage::EntityStore entities(orm);
            if (auto record = entities.load(kRoleTable, role_key(request.role_id))) {
                if (auto decoded = decode_role_record(*record)) {
                    payload_response = *decoded;
                    if (payload_response.role.player_uid != request.player_uid) {
                        return response_for(message, yuan::rpc::RpcStatus::bad_request, {}, "player uid mismatch");
                    }
                }
            }

            yuan::rpc::Bytes payload;
            (void)encode_binary(payload_response, payload);
            return response_for(message, yuan::rpc::RpcStatus::ok, std::move(payload));
        }

        yuan::rpc::Response save_role_response(const yuan::rpc::Message &message,
                                               const SSPlayerDbSaveRoleRequest &request,
                                               storage::RedisOrmStore orm)
        {
            storage::EntityStore entities(orm);
            const auto next_version = request.data_version == 0 ? 1 : request.data_version;
            const auto saved = entities.save(role_record(request.role, next_version));
            if (!saved.ok) {
                return response_for(message, yuan::rpc::RpcStatus::unavailable, {}, "failed to save role");
            }

            SSPlayerDbRoleResponse payload_response{true, "ok", true, request.role, next_version};
            yuan::rpc::Bytes payload;
            (void)encode_binary(payload_response, payload);
            return response_for(message, yuan::rpc::RpcStatus::ok, std::move(payload));
        }

        yuan::rpc::Response create_role_response(const yuan::rpc::Message &message,
                                                  const SSPlayerDbCreateRoleRequest &request,
                                                  storage::RedisOrmStore orm)
        {
            storage::EntityStore entities(orm);
            if (entities.load(kRoleTable, role_key(request.role_id))) {
                return response_for(message, yuan::rpc::RpcStatus::bad_request, {}, "role already exists");
            }

            SSPlayerRoleData role{request.player_uid, request.role_id, 1, 0};
            const auto saved = entities.insert(role_record(role, 1));
            if (!saved.ok) {
                return response_for(message, yuan::rpc::RpcStatus::unavailable, {}, "failed to create role");
            }

            SSPlayerDbRoleResponse payload_response{true, "ok", true, role, 1};
            yuan::rpc::Bytes payload;
            (void)encode_binary(payload_response, payload);
            return response_for(message, yuan::rpc::RpcStatus::ok, std::move(payload));
        }

        yuan::rpc::Response mail_create_response(const yuan::rpc::Message &message,
                                                 const SSPlayerDbMailCreateRequest &request,
                                                 storage::RedisOrmStore orm)
        {
            if (request.player_uid == 0 || request.role_id == 0 || request.title.empty()) {
                return response_for(message, yuan::rpc::RpcStatus::bad_request, {}, "invalid player mail create request");
            }

            storage::EntityStore entities(orm);
            if (!request.dedupe_key.empty()) {
                if (auto dedupe = entities.load(kMailDedupeTable, mail_dedupe_key(request.player_uid, request.role_id, request.dedupe_key))) {
                    SSPlayerDbMailCreateResponse payload_response{true, "ok", storage::field_u64(dedupe->fields, "mail_id", 0), true, dedupe->version};
                    yuan::rpc::Bytes payload;
                    (void)encode_binary(payload_response, payload);
                    return response_for(message, yuan::rpc::RpcStatus::ok, std::move(payload));
                }
            }

            const auto created_ms = request.now_ms == 0 ? now_ms() : request.now_ms;
            const auto mail_id = created_ms * 1000 + static_cast<std::uint64_t>(message.request_id % 1000);
            SSPlayerDbMailRecord mail;
            mail.mail_id = mail_id;
            mail.player_uid = request.player_uid;
            mail.role_id = request.role_id;
            mail.title = request.title;
            mail.body = request.body;
            mail.detail = request.detail;
            mail.sender = request.sender;
            mail.attachments = request.attachments;
            mail.created_at_ms = created_ms;
            mail.starts_at_ms = request.starts_at_ms;
            mail.expires_at_ms = request.expires_at_ms;
            mail.dedupe_key = request.dedupe_key;
            mail.data_version = 1;

            const auto saved = entities.insert(mail_record(mail));
            if (!saved.ok) {
                return response_for(message, yuan::rpc::RpcStatus::unavailable, {}, "failed to save player mail");
            }

            auto index = load_mail_index(entities, request.player_uid, request.role_id);
            index.mail_ids.push_back(mail_id);
            if (!save_mail_index(entities, request.player_uid, request.role_id, std::move(index))) {
                return response_for(message, yuan::rpc::RpcStatus::unavailable, {}, "failed to save player mail index");
            }

            if (!request.dedupe_key.empty()) {
                (void)entities.insert(storage::EntityRecord{kMailDedupeTable,
                                                             mail_dedupe_key(request.player_uid, request.role_id, request.dedupe_key),
                                                             {{"mail_id", std::to_string(mail_id)}},
                                                             1});
            }

            SSPlayerDbMailCreateResponse payload_response{true, "ok", mail_id, false, 1};
            yuan::rpc::Bytes payload;
            (void)encode_binary(payload_response, payload);
            return response_for(message, yuan::rpc::RpcStatus::ok, std::move(payload));
        }

        yuan::rpc::Response mail_list_response(const yuan::rpc::Message &message,
                                               const SSPlayerDbMailListRequest &request,
                                               storage::RedisOrmStore orm)
        {
            if (request.player_uid == 0 || request.role_id == 0) {
                return response_for(message, yuan::rpc::RpcStatus::bad_request, {}, "player_uid and role_id are required");
            }

            storage::EntityStore entities(orm);
            auto ids = load_mail_index(entities, request.player_uid, request.role_id).mail_ids;
            std::sort(ids.begin(), ids.end(), std::greater<>());
            ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
            SSPlayerDbMailListResponse payload_response;
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
                if (!mail || mail->player_uid != request.player_uid || mail->role_id != request.role_id || !mail_visible_at(*mail, current_ms)) {
                    continue;
                }
                payload_response.mails.push_back(mail_box_item(entities, *mail));
            }

            yuan::rpc::Bytes payload;
            (void)encode_binary(payload_response, payload);
            return response_for(message, yuan::rpc::RpcStatus::ok, std::move(payload));
        }

        yuan::rpc::Response mail_get_response(const yuan::rpc::Message &message,
                                              const SSPlayerDbMailGetRequest &request,
                                              storage::RedisOrmStore orm)
        {
            if (request.player_uid == 0 || request.role_id == 0 || request.mail_id == 0) {
                return response_for(message, yuan::rpc::RpcStatus::bad_request, {}, "player_uid role_id and mail_id are required");
            }

            storage::EntityStore entities(orm);
            SSPlayerDbMailGetResponse payload_response;
            payload_response.ok = true;
            payload_response.message = "ok";
            const auto current_ms = request.now_ms == 0 ? now_ms() : request.now_ms;
            if (auto record = entities.load(kMailTable, mail_key(request.mail_id))) {
                if (auto mail = decode_mail_record(*record); mail && mail->player_uid == request.player_uid && mail->role_id == request.role_id && mail_visible_at(*mail, current_ms)) {
                    payload_response.has_mail = true;
                    payload_response.mail = mail_box_item(entities, *mail);
                }
            }

            yuan::rpc::Bytes payload;
            (void)encode_binary(payload_response, payload);
            return response_for(message, yuan::rpc::RpcStatus::ok, std::move(payload));
        }

        yuan::rpc::Response mail_claim_attachment_response(const yuan::rpc::Message &message,
                                                           const SSPlayerDbMailClaimAttachmentRequest &request,
                                                           storage::RedisOrmStore orm)
        {
            if (request.player_uid == 0 || request.role_id == 0 || request.mail_id == 0) {
                return response_for(message, yuan::rpc::RpcStatus::bad_request, {}, "player_uid role_id and mail_id are required");
            }

            storage::EntityStore entities(orm);
            const auto current_ms = request.now_ms == 0 ? now_ms() : request.now_ms;
            const auto record = entities.load(kMailTable, mail_key(request.mail_id));
            if (!record) {
                SSPlayerDbMailClaimAttachmentResponse payload_response{false, "mail not found", false, request.mail_id, {}, 0};
                yuan::rpc::Bytes payload;
                (void)encode_binary(payload_response, payload);
                return response_for(message, yuan::rpc::RpcStatus::ok, std::move(payload));
            }

            auto mail = decode_mail_record(*record);
            if (!mail || mail->player_uid != request.player_uid || mail->role_id != request.role_id || !mail_visible_at(*mail, current_ms)) {
                SSPlayerDbMailClaimAttachmentResponse payload_response{false, "mail not available", false, request.mail_id, {}, 0};
                yuan::rpc::Bytes payload;
                (void)encode_binary(payload_response, payload);
                return response_for(message, yuan::rpc::RpcStatus::ok, std::move(payload));
            }

            if (mail->attachments.empty()) {
                SSPlayerDbMailClaimAttachmentResponse payload_response{false, "mail has no attachments", false, request.mail_id, {}, 0};
                yuan::rpc::Bytes payload;
                (void)encode_binary(payload_response, payload);
                return response_for(message, yuan::rpc::RpcStatus::ok, std::move(payload));
            }

            auto state = load_mail_state(entities, request.player_uid, request.role_id, request.mail_id);
            if (state.attachment_claimed) {
                SSPlayerDbMailClaimAttachmentResponse payload_response{true, "already claimed", false, request.mail_id, {}, state.claimed_at_ms};
                yuan::rpc::Bytes payload;
                (void)encode_binary(payload_response, payload);
                return response_for(message, yuan::rpc::RpcStatus::ok, std::move(payload));
            }

            state.attachment_claimed = true;
            state.state = mail_state::attachment_claimed;
            state.claimed_at_ms = current_ms;
            if (!save_mail_state(entities, request.player_uid, request.role_id, request.mail_id, state)) {
                return response_for(message, yuan::rpc::RpcStatus::unavailable, {}, "failed to save player mail state");
            }

            SSPlayerDbMailClaimAttachmentResponse payload_response{true, "ok", true, request.mail_id, mail->attachments, state.claimed_at_ms};
            yuan::rpc::Bytes payload;
            (void)encode_binary(payload_response, payload);
            return response_for(message, yuan::rpc::RpcStatus::ok, std::move(payload));
        }

        template <typename Fn>
        bool submit_deferred(PlayerDbMsgContext &context, const yuan::rpc::Message &message, Fn &&fn)
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

        yuan::rpc::Response handle_player_db_load_role(PlayerDbMsgContext &context, const yuan::rpc::Message &message)
        {
            const auto request = decode_binary<SSPlayerDbLoadRoleRequest>(message.payload);
            if (!request || request->player_uid == 0 || request->role_id == 0) {
                return response_for(message, yuan::rpc::RpcStatus::bad_request, {}, "invalid player db load role request");
            }

            if (!ensure_redis(context)) {
                return response_for(message, yuan::rpc::RpcStatus::unavailable, {}, "redis unavailable");
            }

            if (submit_deferred(context, message, [message, request = *request](yuan::redis::RedisClient &redis) {
                    return load_role_response(message, request, make_store(redis));
                })) {
                return deferred_response_for(message);
            }

            return load_role_response(message, *request, make_store(context));
        }

        yuan::rpc::Response handle_player_db_save_role(PlayerDbMsgContext &context, const yuan::rpc::Message &message)
        {
            const auto request = decode_binary<SSPlayerDbSaveRoleRequest>(message.payload);
            if (!request || request->role.player_uid == 0 || request->role.role_id == 0) {
                return response_for(message, yuan::rpc::RpcStatus::bad_request, {}, "invalid player db save role request");
            }

            if (!ensure_redis(context)) {
                return response_for(message, yuan::rpc::RpcStatus::unavailable, {}, "redis unavailable");
            }

            if (submit_deferred(context, message, [message, request = *request](yuan::redis::RedisClient &redis) {
                    return save_role_response(message, request, make_store(redis));
                })) {
                return deferred_response_for(message);
            }

            return save_role_response(message, *request, make_store(context));
        }

        yuan::rpc::Response handle_player_db_create_role(PlayerDbMsgContext &context, const yuan::rpc::Message &message)
        {
            const auto request = decode_binary<SSPlayerDbCreateRoleRequest>(message.payload);
            if (!request || request->player_uid == 0 || request->role_id == 0 || request->name.empty()) {
                return response_for(message, yuan::rpc::RpcStatus::bad_request, {}, "invalid player db create role request");
            }

            if (!ensure_redis(context)) {
                return response_for(message, yuan::rpc::RpcStatus::unavailable, {}, "redis unavailable");
            }

            if (submit_deferred(context, message, [message, request = *request](yuan::redis::RedisClient &redis) {
                    return create_role_response(message, request, make_store(redis));
                })) {
                return deferred_response_for(message);
            }

            return create_role_response(message, *request, make_store(context));
        }

        yuan::rpc::Response handle_player_db_mail_create(PlayerDbMsgContext &context, const yuan::rpc::Message &message)
        {
            const auto request = decode_binary<SSPlayerDbMailCreateRequest>(message.payload);
            if (!request) {
                return response_for(message, yuan::rpc::RpcStatus::bad_request, {}, "invalid player mail create payload");
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

        yuan::rpc::Response handle_player_db_mail_list(PlayerDbMsgContext &context, const yuan::rpc::Message &message)
        {
            const auto request = decode_binary<SSPlayerDbMailListRequest>(message.payload);
            if (!request) {
                return response_for(message, yuan::rpc::RpcStatus::bad_request, {}, "invalid player mail list payload");
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

        yuan::rpc::Response handle_player_db_mail_get(PlayerDbMsgContext &context, const yuan::rpc::Message &message)
        {
            const auto request = decode_binary<SSPlayerDbMailGetRequest>(message.payload);
            if (!request) {
                return response_for(message, yuan::rpc::RpcStatus::bad_request, {}, "invalid player mail get payload");
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

        yuan::rpc::Response handle_player_db_mail_claim_attachment(PlayerDbMsgContext &context, const yuan::rpc::Message &message)
        {
            const auto request = decode_binary<SSPlayerDbMailClaimAttachmentRequest>(message.payload);
            if (!request) {
                return response_for(message, yuan::rpc::RpcStatus::bad_request, {}, "invalid player mail claim payload");
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

    bool register_player_db_msg(yuan::rpc::Server &server, PlayerDbMsgContext &context)
    {
        const bool load_role = server.register_handler(game_route::player_db_load_role(), std::bind_front(handle_player_db_load_role, std::ref(context)));
        const bool save_role = server.register_handler(game_route::player_db_save_role(), std::bind_front(handle_player_db_save_role, std::ref(context)));
        const bool create_role = server.register_handler(game_route::player_db_create_role(), std::bind_front(handle_player_db_create_role, std::ref(context)));
        const bool create_mail = server.register_handler(game_route::player_db_mail_create(), std::bind_front(handle_player_db_mail_create, std::ref(context)));
        const bool list_mail = server.register_handler(game_route::player_db_mail_list(), std::bind_front(handle_player_db_mail_list, std::ref(context)));
        const bool get_mail = server.register_handler(game_route::player_db_mail_get(), std::bind_front(handle_player_db_mail_get, std::ref(context)));
        const bool claim_mail = server.register_handler(game_route::player_db_mail_claim_attachment(), std::bind_front(handle_player_db_mail_claim_attachment, std::ref(context)));

        return load_role && save_role && create_role && create_mail && list_mail && get_mail && claim_mail;
    }
}
