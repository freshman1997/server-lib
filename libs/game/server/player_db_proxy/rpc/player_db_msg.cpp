#include "player_db_proxy/rpc/player_db_msg.h"

#include "common/storage/entity_store.h"
#include "common/storage/redis_orm_store.h"

namespace yuan::game::server
{
    namespace
    {
        constexpr const char *kRoleTable = "player_role";

        std::string role_key(RoleId role_id)
        {
            return std::to_string(role_id);
        }

        storage::EntityRecord role_record(const SSPlayerRoleData &role, std::uint64_t version)
        {
            return storage::EntityRecord{kRoleTable,
                                         role_key(role.role_id),
                                         {{"player_uid", std::to_string(role.player_uid)},
                                          {"role_id", std::to_string(role.role_id)},
                                          {"level", std::to_string(role.level)},
                                          {"exp", std::to_string(role.exp)}},
                                         version};
        }

        std::optional<SSPlayerDbRoleResponse> decode_role_record(const storage::EntityRecord &record)
        {
            SSPlayerDbRoleResponse response;
            response.ok = true;
            response.message = "ok";
            response.has_role = true;
            response.role.player_uid = storage::field_u64(record.fields, "player_uid", 0);
            response.role.role_id = storage::field_u64(record.fields, "role_id", 0);
            response.role.level = storage::field_u32(record.fields, "level", 1);
            response.role.exp = storage::field_u64(record.fields, "exp", 0);
            response.data_version = record.version;
            if (response.role.player_uid == 0 || response.role.role_id == 0) {
                return std::nullopt;
            }
            return response;
        }

        bool ensure_redis(PlayerDbMsgContext &context)
        {
            return context.redis && context.redis->ensure_connected();
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
    }

    bool register_player_db_msg(yuan::rpc::Server &server, PlayerDbMsgContext &context)
    {
        const bool load_role = server.register_handler(game_route::player_db_load_role(), [&context](const yuan::rpc::Message &message) {
            const auto request = decode_binary<SSPlayerDbLoadRoleRequest>(message.payload);
            if (!request || request->player_uid == 0 || request->role_id == 0) {
                return response_for(message, yuan::rpc::RpcStatus::bad_request, {}, "invalid player db load role request");
            }
            if (!ensure_redis(context)) {
                return response_for(message, yuan::rpc::RpcStatus::unavailable, {}, "redis unavailable");
            }
            SSPlayerDbRoleResponse payload_response;
            payload_response.ok = true;
            payload_response.message = "ok";
            storage::RedisOrmStore orm(context.redis, "game:player_db:");
            storage::EntityStore entities(orm);
            if (auto record = entities.load(kRoleTable, role_key(request->role_id))) {
                if (auto decoded = decode_role_record(*record)) {
                    payload_response = *decoded;
                    if (payload_response.role.player_uid != request->player_uid) {
                        return response_for(message, yuan::rpc::RpcStatus::bad_request, {}, "player uid mismatch");
                    }
                }
            }
            yuan::rpc::Bytes payload;
            (void)encode_binary(payload_response, payload);
            return response_for(message, yuan::rpc::RpcStatus::ok, std::move(payload));
        });

        const bool save_role = server.register_handler(game_route::player_db_save_role(), [&context](const yuan::rpc::Message &message) {
            const auto request = decode_binary<SSPlayerDbSaveRoleRequest>(message.payload);
            if (!request || request->role.player_uid == 0 || request->role.role_id == 0) {
                return response_for(message, yuan::rpc::RpcStatus::bad_request, {}, "invalid player db save role request");
            }
            if (!ensure_redis(context)) {
                return response_for(message, yuan::rpc::RpcStatus::unavailable, {}, "redis unavailable");
            }
            storage::RedisOrmStore orm(context.redis, "game:player_db:");
            storage::EntityStore entities(orm);
            const auto next_version = request->data_version == 0 ? 1 : request->data_version;
            const auto saved = entities.save(role_record(request->role, next_version));
            if (!saved.ok) {
                return response_for(message, yuan::rpc::RpcStatus::unavailable, {}, "failed to save role");
            }
            SSPlayerDbRoleResponse payload_response{true, "ok", true, request->role, next_version};
            yuan::rpc::Bytes payload;
            (void)encode_binary(payload_response, payload);
            return response_for(message, yuan::rpc::RpcStatus::ok, std::move(payload));
        });

        return load_role && save_role;
    }
}
