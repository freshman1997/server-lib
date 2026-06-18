#include "world_db_proxy/rpc/world_db_msg.h"

#include "common/metadata_keys.h"
#include "common/storage/entity_store.h"
#include "common/storage/redis_orm_store.h"

#include <functional>
#include <sstream>

namespace yuan::game::server
{
    namespace
    {
        constexpr const char *kRoleLocationTable = "role_location";
        constexpr const char *kPlayerRolesTable = "player_roles";
        constexpr const char *kRoleInfoTable = "role_info";
        constexpr const char *kOwnershipTable = "ownership";

        std::string role_location_key(RoleId role_id)
        {
            return std::to_string(role_id);
        }

        std::string player_roles_key(PlayerUid player_uid)
        {
            return std::to_string(player_uid);
        }

        std::string role_info_key(RoleId role_id)
        {
            return std::to_string(role_id);
        }

        std::string ownership_key(RoleId role_id)
        {
            return std::to_string(role_id);
        }

        storage::EntityRecord location_record(const SSWorldDbRoleLocationSetRequest &request, std::uint64_t version)
        {
            return storage::EntityRecord{kRoleLocationTable,
                                         role_location_key(request.role_id),
                                         {{"player_uid", std::to_string(request.player_uid)},
                                          {"role_id", std::to_string(request.role_id)},
                                          {"zone_service_id", std::to_string(request.zone_service_id)},
                                          {"gateway_session_id", std::to_string(request.gateway_session_id)}},
                                         version};
        }

        std::optional<SSWorldDbRoleLocationResponse> decode_location_record(const storage::EntityRecord &record)
        {
            SSWorldDbRoleLocationResponse response;
            response.ok = true;
            response.message = "ok";
            response.has_location = true;
            response.player_uid = storage::field_u64(record.fields, "player_uid", 0);
            response.role_id = storage::field_u64(record.fields, "role_id", 0);
            response.zone_service_id = storage::field_u64(record.fields, "zone_service_id", 0);
            response.gateway_session_id = storage::field_u64(record.fields, "gateway_session_id", 0);
            response.data_version = record.version;
            if (response.role_id == 0) {
                return std::nullopt;
            }
            return response;
        }

        storage::EntityRecord player_roles_record(PlayerUid player_uid, const std::vector<RoleId> &role_ids, std::uint64_t version)
        {
            storage::EntityRecord record{kPlayerRolesTable,
                                         player_roles_key(player_uid),
                                         {{"player_uid", std::to_string(player_uid)}},
                                         version};
            (void)encode_binary(SSWorldDbRoleIdList{role_ids}, record.object_blob);
            return record;
        }

        storage::EntityRecord role_info_record(PlayerUid player_uid, const SSPlayerRoleInfo &role, std::uint64_t version)
        {
            storage::EntityRecord record{kRoleInfoTable,
                                         role_info_key(role.role_id),
                                         {{"player_uid", std::to_string(player_uid)},
                                          {"role_id", std::to_string(role.role_id)},
                                          {"name", role.name},
                                          {"level", std::to_string(role.level)},
                                          {"world_service_id", std::to_string(role.world_service_id)},
                                          {"zone_service_id", std::to_string(role.zone_service_id)}},
                                         version};
            (void)encode_binary(role, record.object_blob);
            return record;
        }

        std::vector<RoleId> decode_role_ids_record(const storage::EntityRecord &record)
        {
            if (!record.object_blob.empty()) {
                if (auto decoded = decode_binary<SSWorldDbRoleIdList>(record.object_blob)) {
                    return decoded->role_ids;
                }
            }
            if (const auto it = record.fields.find("role_ids"); it != record.fields.end()) {
                std::vector<RoleId> role_ids;
                std::stringstream stream(it->second);
                std::string item;
                while (std::getline(stream, item, ',')) {
                    if (!item.empty()) {
                        role_ids.push_back(static_cast<RoleId>(std::stoull(item)));
                    }
                }
                return role_ids;
            }
            return {};
        }

        std::optional<SSPlayerRoleInfo> decode_role_info_record(const storage::EntityRecord &record)
        {
            if (!record.object_blob.empty()) {
                if (auto decoded = decode_binary<SSPlayerRoleInfo>(record.object_blob)) {
                    return *decoded;
                }
            }
            SSPlayerRoleInfo role;
            role.role_id = storage::field_u64(record.fields, "role_id", 0);
            if (const auto it = record.fields.find("name"); it != record.fields.end()) {
                role.name = it->second;
            }
            role.level = storage::field_u32(record.fields, "level", 1);
            role.world_service_id = storage::field_u64(record.fields, "world_service_id", 0);
            role.zone_service_id = storage::field_u64(record.fields, "zone_service_id", 0);
            if (role.role_id == 0) {
                return std::nullopt;
            }
            return role;
        }

        SSWorldDbOwnershipCompareAndSetResponse decode_ownership_record(RoleId role_id, const storage::EntityRecord &record)
        {
            SSWorldDbOwnershipCompareAndSetResponse response;
            response.ok = true;
            response.message = "ok";
            response.has_owner = true;
            response.role_id = role_id;
            response.zone_service_id = storage::field_u64(record.fields, "zone_service_id", 0);
            response.gateway_session_id = storage::field_u64(record.fields, "gateway_session_id", 0);
            response.data_version = record.version;
            if (response.zone_service_id == 0) {
                response.has_owner = false;
            }
            return response;
        }

        storage::EntityRecord ownership_record(const SSWorldDbOwnershipCompareAndSetRequest &request, std::uint64_t version)
        {
            return storage::EntityRecord{kOwnershipTable,
                                         ownership_key(request.role_id),
                                         {{"role_id", std::to_string(request.role_id)},
                                          {"zone_service_id", std::to_string(request.next_zone_service_id)},
                                          {"gateway_session_id", std::to_string(request.next_gateway_session_id)}},
                                         version};
        }

        bool ensure_redis(WorldDbMsgContext &context)
        {
            return context.redis_pool || (context.redis && context.redis->ensure_connected());
        }

        storage::RedisOrmStore make_store(WorldDbMsgContext &context)
        {
            if (context.redis_pool) {
                return storage::RedisOrmStore(context.redis_pool, "game:world_db:");
            }
            return storage::RedisOrmStore(context.redis, "game:world_db:");
        }

        storage::RedisOrmStore make_store(yuan::redis::RedisClient &redis)
        {
            return storage::RedisOrmStore(std::shared_ptr<yuan::redis::RedisClient>(&redis, [](yuan::redis::RedisClient *) {}), "game:world_db:");
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

        yuan::rpc::Response make_response(const yuan::rpc::Message &message, yuan::rpc::RpcStatus status, yuan::rpc::Bytes payload = {}, std::string error = {})
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
            auto response = make_response(message, yuan::rpc::RpcStatus::ok);
            response.metadata[game_metadata_key::rpc_defer_response] = "1";
            return response;
        }

        yuan::rpc::Response role_location_get_response(const yuan::rpc::Message &message,
                                                       const SSWorldDbRoleLocationGetRequest &request,
                                                       storage::RedisOrmStore orm)
        {
            SSWorldDbRoleLocationResponse body;
            body.ok = true;
            body.message = "ok";
            storage::EntityStore entities(orm);
            if (auto record = entities.load(kRoleLocationTable, role_location_key(request.role_id))) {
                if (auto decoded = decode_location_record(*record)) {
                    body = *decoded;
                }
            }
            yuan::rpc::Bytes payload;
            (void)encode_binary(body, payload);
            return make_response(message, yuan::rpc::RpcStatus::ok, std::move(payload));
        }

        yuan::rpc::Response role_location_set_response(const yuan::rpc::Message &message,
                                                       const SSWorldDbRoleLocationSetRequest &request,
                                                       storage::RedisOrmStore orm)
        {
            storage::EntityStore entities(orm);
            const auto next_version = request.data_version == 0 ? 1 : request.data_version;
            if (request.zone_service_id == 0) {
                (void)entities.remove(kRoleLocationTable, role_location_key(request.role_id));
                SSWorldDbRoleLocationResponse body{true, "ok", false, request.player_uid, request.role_id, 0, 0, next_version};
                yuan::rpc::Bytes payload;
                (void)encode_binary(body, payload);
                return make_response(message, yuan::rpc::RpcStatus::ok, std::move(payload));
            }
            const auto saved = entities.save(location_record(request, next_version));
            if (!saved.ok) {
                return make_response(message, yuan::rpc::RpcStatus::unavailable, {}, "failed to save role location");
            }
            SSWorldDbRoleLocationResponse body{true, "ok", true, request.player_uid, request.role_id, request.zone_service_id, request.gateway_session_id, next_version};
            yuan::rpc::Bytes payload;
            (void)encode_binary(body, payload);
            return make_response(message, yuan::rpc::RpcStatus::ok, std::move(payload));
        }

        yuan::rpc::Response player_roles_get_response(const yuan::rpc::Message &message,
                                                       const SSWorldDbPlayerRolesGetRequest &request,
                                                       storage::RedisOrmStore orm)
        {
            storage::EntityStore entities(orm);
            SSWorldDbPlayerRolesResponse body;
            body.ok = true;
            body.message = "ok";
            body.player_uid = request.player_uid;
            if (auto record = entities.load(kPlayerRolesTable, player_roles_key(request.player_uid))) {
                body.data_version = record->version;
                for (const auto role_id : decode_role_ids_record(*record)) {
                    if (auto role_record = entities.load(kRoleInfoTable, role_info_key(role_id))) {
                        if (auto role = decode_role_info_record(*role_record)) {
                            body.roles.push_back(*role);
                        }
                    }
                }
            } else {
                body.missing_role_list = true;
            }

            yuan::rpc::Bytes payload;
            (void)encode_binary(body, payload);
            return make_response(message, yuan::rpc::RpcStatus::ok, std::move(payload));
        }

        yuan::rpc::Response player_roles_save_response(const yuan::rpc::Message &message,
                                                        const SSWorldDbPlayerRolesSaveRequest &request,
                                                        storage::RedisOrmStore orm)
        {
            storage::EntityStore entities(orm);
            std::vector<RoleId> role_ids;
            role_ids.reserve(request.roles.size());
            const auto next_version = request.data_version == 0 ? 1 : request.data_version;
            for (const auto &role : request.roles) {
                if (role.role_id == 0) {
                    continue;
                }
                role_ids.push_back(role.role_id);
                const auto saved_role = entities.save(role_info_record(request.player_uid, role, next_version));
                if (!saved_role.ok) {
                    return make_response(message, yuan::rpc::RpcStatus::unavailable, {}, "failed to save role info");
                }
            }
            const auto saved_roles = entities.save(player_roles_record(request.player_uid, role_ids, next_version));
            if (!saved_roles.ok) {
                return make_response(message, yuan::rpc::RpcStatus::unavailable, {}, "failed to save player roles");
            }
            SSWorldDbPlayerRolesResponse body{true, "ok", request.player_uid, request.roles, false, false, next_version};
            yuan::rpc::Bytes payload;
            (void)encode_binary(body, payload);
            return make_response(message, yuan::rpc::RpcStatus::ok, std::move(payload));
        }

        yuan::rpc::Response ownership_compare_and_set_response(const yuan::rpc::Message &message,
                                                               const SSWorldDbOwnershipCompareAndSetRequest &request,
                                                               storage::RedisOrmStore orm)
        {
            storage::EntityStore entities(orm);
            SSWorldDbOwnershipCompareAndSetResponse body;
            body.ok = true;
            body.message = "ok";
            body.role_id = request.role_id;

            if (const auto current = entities.load(kOwnershipTable, ownership_key(request.role_id))) {
                body = decode_ownership_record(request.role_id, *current);
                body.applied = false;
            }

            bool stale_update = false;
            if (request.next_zone_service_id == 0 && request.source_zone_service_id != 0 && body.zone_service_id != 0 && body.zone_service_id != request.source_zone_service_id) {
                stale_update = true;
            }
            if (request.next_zone_service_id == 0 && request.expected_gateway_session_id != 0 && body.gateway_session_id != 0 && body.gateway_session_id != request.expected_gateway_session_id) {
                stale_update = true;
            }
            if (!stale_update) {
                const auto next_version = body.data_version + 1;
                if (request.next_zone_service_id == 0) {
                    (void)entities.remove(kOwnershipTable, ownership_key(request.role_id));
                    body.has_owner = false;
                    body.zone_service_id = 0;
                    body.gateway_session_id = 0;
                    body.data_version = next_version;
                    body.applied = true;
                } else {
                    const auto saved = entities.save(ownership_record(request, next_version));
                    if (!saved.ok) {
                        return make_response(message, yuan::rpc::RpcStatus::unavailable, {}, "failed to save ownership");
                    }
                    body.has_owner = true;
                    body.zone_service_id = request.next_zone_service_id;
                    body.gateway_session_id = request.next_gateway_session_id;
                    body.data_version = next_version;
                    body.applied = true;
                }
            }

            yuan::rpc::Bytes payload;
            (void)encode_binary(body, payload);
            return make_response(message, yuan::rpc::RpcStatus::ok, std::move(payload));
        }

        template <typename Fn>
        bool submit_deferred(WorldDbMsgContext &context, const yuan::rpc::Message &message, Fn &&fn)
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

        yuan::rpc::Response handle_world_db_role_location_get(WorldDbMsgContext &context, const yuan::rpc::Message &message)
        {
            const auto request = decode_binary<SSWorldDbRoleLocationGetRequest>(message.payload);
            if (!request || request->role_id == 0) {
                return make_response(message, yuan::rpc::RpcStatus::bad_request, {}, "invalid world db role location get request");
            }
            if (!ensure_redis(context)) {
                return make_response(message, yuan::rpc::RpcStatus::unavailable, {}, "redis unavailable");
            }
            if (submit_deferred(context, message, [message, request = *request](yuan::redis::RedisClient &redis) {
                    return role_location_get_response(message, request, make_store(redis));
                })) {
                return deferred_response_for(message);
            }
            return role_location_get_response(message, *request, make_store(context));
        }

        yuan::rpc::Response handle_world_db_role_location_set(WorldDbMsgContext &context, const yuan::rpc::Message &message)
        {
            const auto request = decode_binary<SSWorldDbRoleLocationSetRequest>(message.payload);
            if (!request || request->role_id == 0) {
                return make_response(message, yuan::rpc::RpcStatus::bad_request, {}, "invalid world db role location set request");
            }
            if (!ensure_redis(context)) {
                return make_response(message, yuan::rpc::RpcStatus::unavailable, {}, "redis unavailable");
            }
            if (submit_deferred(context, message, [message, request = *request](yuan::redis::RedisClient &redis) {
                    return role_location_set_response(message, request, make_store(redis));
                })) {
                return deferred_response_for(message);
            }
            return role_location_set_response(message, *request, make_store(context));
        }

        yuan::rpc::Response handle_world_db_player_roles_get(WorldDbMsgContext &context, const yuan::rpc::Message &message)
        {
            const auto request = decode_binary<SSWorldDbPlayerRolesGetRequest>(message.payload);
            if (!request || request->player_uid == 0) {
                return make_response(message, yuan::rpc::RpcStatus::bad_request, {}, "invalid world db player roles get request");
            }
            if (!ensure_redis(context)) {
                return make_response(message, yuan::rpc::RpcStatus::unavailable, {}, "redis unavailable");
            }
            if (submit_deferred(context, message, [message, request = *request](yuan::redis::RedisClient &redis) {
                    return player_roles_get_response(message, request, make_store(redis));
                })) {
                return deferred_response_for(message);
            }
            return player_roles_get_response(message, *request, make_store(context));
        }

        yuan::rpc::Response handle_world_db_player_roles_save(WorldDbMsgContext &context, const yuan::rpc::Message &message)
        {
            const auto request = decode_binary<SSWorldDbPlayerRolesSaveRequest>(message.payload);
            if (!request || request->player_uid == 0) {
                return make_response(message, yuan::rpc::RpcStatus::bad_request, {}, "invalid world db player roles save request");
            }
            if (!ensure_redis(context)) {
                return make_response(message, yuan::rpc::RpcStatus::unavailable, {}, "redis unavailable");
            }
            if (submit_deferred(context, message, [message, request = *request](yuan::redis::RedisClient &redis) {
                    return player_roles_save_response(message, request, make_store(redis));
                })) {
                return deferred_response_for(message);
            }
            return player_roles_save_response(message, *request, make_store(context));
        }

        yuan::rpc::Response handle_world_db_ownership_compare_and_set(WorldDbMsgContext &context, const yuan::rpc::Message &message)
        {
            const auto request = decode_binary<SSWorldDbOwnershipCompareAndSetRequest>(message.payload);
            if (!request || request->role_id == 0) {
                return make_response(message, yuan::rpc::RpcStatus::bad_request, {}, "invalid world db ownership compare_and_set request");
            }
            if (!ensure_redis(context)) {
                return make_response(message, yuan::rpc::RpcStatus::unavailable, {}, "redis unavailable");
            }
            if (submit_deferred(context, message, [message, request = *request](yuan::redis::RedisClient &redis) {
                    return ownership_compare_and_set_response(message, request, make_store(redis));
                })) {
                return deferred_response_for(message);
            }
            return ownership_compare_and_set_response(message, *request, make_store(context));
        }
    }

    bool register_world_db_msg(yuan::rpc::Server &server, WorldDbMsgContext &context)
    {
        const bool get_registered = server.register_handler(game_route::world_db_role_location_get(), std::bind_front(handle_world_db_role_location_get, std::ref(context)));
        const bool set_registered = server.register_handler(game_route::world_db_role_location_set(), std::bind_front(handle_world_db_role_location_set, std::ref(context)));
        const bool roles_get_registered = server.register_handler(game_route::world_db_player_roles_get(), std::bind_front(handle_world_db_player_roles_get, std::ref(context)));
        const bool roles_save_registered = server.register_handler(game_route::world_db_player_roles_save(), std::bind_front(handle_world_db_player_roles_save, std::ref(context)));
        const bool ownership_cas_registered = server.register_handler(game_route::world_db_ownership_compare_and_set(), std::bind_front(handle_world_db_ownership_compare_and_set, std::ref(context)));

        return get_registered && set_registered && roles_get_registered && roles_save_registered && ownership_cas_registered;
    }
}
