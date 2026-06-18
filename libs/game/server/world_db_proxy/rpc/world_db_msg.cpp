#include "world_db_proxy/rpc/world_db_msg.h"

#include "common/storage/entity_store.h"
#include "common/storage/redis_orm_store.h"

namespace yuan::game::server
{
    namespace
    {
        constexpr const char *kRoleLocationTable = "role_location";

        std::string role_location_key(RoleId role_id)
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

        bool ensure_redis(WorldDbMsgContext &context)
        {
            return context.redis && context.redis->ensure_connected();
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
    }

    bool register_world_db_msg(yuan::rpc::Server &server, WorldDbMsgContext &context)
    {
        const bool get_registered = server.register_handler(game_route::world_db_role_location_get(), [&context](const yuan::rpc::Message &message) {
            const auto request = decode_binary<SSWorldDbRoleLocationGetRequest>(message.payload);
            if (!request || request->role_id == 0) {
                return make_response(message, yuan::rpc::RpcStatus::bad_request, {}, "invalid world db role location get request");
            }
            if (!ensure_redis(context)) {
                return make_response(message, yuan::rpc::RpcStatus::unavailable, {}, "redis unavailable");
            }
            SSWorldDbRoleLocationResponse body;
            body.ok = true;
            body.message = "ok";
            storage::RedisOrmStore orm(context.redis, "game:world_db:");
            storage::EntityStore entities(orm);
            if (auto record = entities.load(kRoleLocationTable, role_location_key(request->role_id))) {
                if (auto decoded = decode_location_record(*record)) {
                    body = *decoded;
                }
            }
            yuan::rpc::Bytes payload;
            (void)encode_binary(body, payload);
            return make_response(message, yuan::rpc::RpcStatus::ok, std::move(payload));
        });

        const bool set_registered = server.register_handler(game_route::world_db_role_location_set(), [&context](const yuan::rpc::Message &message) {
            const auto request = decode_binary<SSWorldDbRoleLocationSetRequest>(message.payload);
            if (!request || request->role_id == 0) {
                return make_response(message, yuan::rpc::RpcStatus::bad_request, {}, "invalid world db role location set request");
            }
            if (!ensure_redis(context)) {
                return make_response(message, yuan::rpc::RpcStatus::unavailable, {}, "redis unavailable");
            }
            storage::RedisOrmStore orm(context.redis, "game:world_db:");
            storage::EntityStore entities(orm);
            const auto next_version = request->data_version == 0 ? 1 : request->data_version;
            if (request->zone_service_id == 0) {
                (void)entities.remove(kRoleLocationTable, role_location_key(request->role_id));
                SSWorldDbRoleLocationResponse body{true, "ok", false, request->player_uid, request->role_id, 0, 0, next_version};
                yuan::rpc::Bytes payload;
                (void)encode_binary(body, payload);
                return make_response(message, yuan::rpc::RpcStatus::ok, std::move(payload));
            }
            const auto saved = entities.save(location_record(*request, next_version));
            if (!saved.ok) {
                return make_response(message, yuan::rpc::RpcStatus::unavailable, {}, "failed to save role location");
            }
            SSWorldDbRoleLocationResponse body{true, "ok", true, request->player_uid, request->role_id, request->zone_service_id, request->gateway_session_id, next_version};
            yuan::rpc::Bytes payload;
            (void)encode_binary(body, payload);
            return make_response(message, yuan::rpc::RpcStatus::ok, std::move(payload));
        });

        return get_registered && set_registered;
    }
}
