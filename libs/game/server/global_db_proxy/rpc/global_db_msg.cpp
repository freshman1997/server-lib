#include "global_db_proxy/rpc/global_db_msg.h"

#include "common/storage/entity_store.h"
#include "common/storage/redis_orm_store.h"

namespace yuan::game::server
{
    namespace
    {
        constexpr const char *kConfigTable = "config";

        bool valid_config_key(const std::string &key)
        {
            return !key.empty() && key.size() <= 128;
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

        bool ensure_redis(GlobalDbMsgContext &context)
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

    bool register_global_db_msg(yuan::rpc::Server &server, GlobalDbMsgContext &context)
    {
        const bool get_config = server.register_handler(game_route::global_db_config_get(), [&context](const yuan::rpc::Message &message) {
            const auto request = decode_binary<SSGlobalDbConfigGetRequest>(message.payload);
            if (!request || !valid_config_key(request->key)) {
                return response_for(message, yuan::rpc::RpcStatus::bad_request, {}, "invalid global config get request");
            }
            if (!ensure_redis(context)) {
                return response_for(message, yuan::rpc::RpcStatus::unavailable, {}, "redis unavailable");
            }
            SSGlobalDbConfigResponse payload_response;
            payload_response.ok = true;
            payload_response.message = "ok";
            payload_response.key = request->key;
            storage::RedisOrmStore orm(context.redis, "game:global_db:");
            storage::EntityStore entities(orm);
            if (auto record = entities.load(kConfigTable, request->key)) {
                payload_response = decode_config_record(*record);
            }
            yuan::rpc::Bytes payload;
            (void)encode_binary(payload_response, payload);
            return response_for(message, yuan::rpc::RpcStatus::ok, std::move(payload));
        });

        const bool set_config = server.register_handler(game_route::global_db_config_set(), [&context](const yuan::rpc::Message &message) {
            const auto request = decode_binary<SSGlobalDbConfigSetRequest>(message.payload);
            if (!request || !valid_config_key(request->key)) {
                return response_for(message, yuan::rpc::RpcStatus::bad_request, {}, "invalid global config set request");
            }
            if (!ensure_redis(context)) {
                return response_for(message, yuan::rpc::RpcStatus::unavailable, {}, "redis unavailable");
            }
            storage::RedisOrmStore orm(context.redis, "game:global_db:");
            storage::EntityStore entities(orm);
            const auto next_version = request->data_version == 0 ? 1 : request->data_version;
            const auto saved = entities.save(config_record(*request, next_version));
            if (!saved.ok) {
                return response_for(message, yuan::rpc::RpcStatus::unavailable, {}, "failed to save global config");
            }
            SSGlobalDbConfigResponse payload_response{true, "ok", true, request->key, request->value, next_version};
            yuan::rpc::Bytes payload;
            (void)encode_binary(payload_response, payload);
            return response_for(message, yuan::rpc::RpcStatus::ok, std::move(payload));
        });

        return get_config && set_config;
    }
}
