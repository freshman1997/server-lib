#include "gateway/rpc/gateway_msg_client.h"

#include "common/metadata_keys.h"
#include "common/rpc_network.h"
#include "base/time.h"

#include <cstdlib>

namespace yuan::game::server
{
    namespace
    {
        std::uint64_t connection_id_from(const yuan::rpc::Message &message)
        {
            const auto it = message.metadata.find(rpc_network::metadata_key::connection_id);
            if (it == message.metadata.end()) {
                return 0;
            }
            return static_cast<std::uint64_t>(std::strtoull(it->second.c_str(), nullptr, 10));
        }

        std::uint64_t metadata_u64(const yuan::rpc::Metadata &metadata, const char *key)
        {
            const auto it = metadata.find(key);
            if (it == metadata.end()) {
                return 0;
            }
            return static_cast<std::uint64_t>(std::strtoull(it->second.c_str(), nullptr, 10));
        }

        std::uint64_t metadata_u64(const yuan::rpc::Message &message, const char *key)
        {
            return metadata_u64(message.metadata, key);
        }

        std::uint64_t metadata_u64(const yuan::rpc::Response &response, const char *key)
        {
            return metadata_u64(response.metadata, key);
        }

        bool has_internal_secret(const yuan::rpc::Message &message, std::uint64_t expected_secret)
        {
            const auto it = message.metadata.find(game_metadata_key::gateway_internal_secret);
            if (it == message.metadata.end()) {
                return false;
            }
            try {
                return static_cast<std::uint64_t>(std::stoull(it->second)) == expected_secret;
            } catch (...) {
                return false;
            }
        }

        void reject_not_logged_in(yuan::rpc::Response &response)
        {
            response.status = yuan::rpc::RpcStatus::not_found;
            response.error = "role is not logged in";
            response.metadata[rpc_network::metadata_key::close_connection] = "1";
        }

        void strip_gateway_internal_metadata(yuan::rpc::Response &response)
        {
            response.metadata.erase(game_metadata_key::gateway_zone_service_id);
            response.metadata.erase(game_metadata_key::gateway_session_id);
        }
    }

    SSGatewayInfo gateway_public_info(const GatewayMsgContext &context)
    {
        SSGatewayInfo info;
        info.service_id = context.address.service.pack();
        info.host = context.public_host;
        info.port = context.public_port;
        info.name = context.address.name;
        return info;
    }

    bool register_gateway_msg_client(yuan::rpc::Server &server, GatewayMsgContext &context, GatewayZoneSelector select_login_zone, GatewayZoneForwarder forward_to_zone, GatewayClientPusher push_to_client, GatewaySessionCloser close_session)
    {
        const bool login_registered = server.register_handler(game_route::gateway_login(), [&context, select_login_zone, forward_to_zone](const yuan::rpc::Message &message) {
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());

            const auto connection_id = connection_id_from(message);
            const auto client_login = decode_binary<CSLoginRequest>(message.payload);
            if (!client_login || client_login->login_token_id.empty()) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid gateway login request";
                response.metadata[rpc_network::metadata_key::close_connection] = "1";
                return response;
            }

            const auto decoded_zone_service_id = decode_login_token_id(client_login->login_token_id, yuan::base::time::steady_now_ms(), context.login_token_secret);
            if (!decoded_zone_service_id) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid gateway login token";
                response.metadata[rpc_network::metadata_key::close_connection] = "1";
                return response;
            }

            const auto zone_service_id = *decoded_zone_service_id;
            if (zone_service_id == 0) {
                response.status = yuan::rpc::RpcStatus::unavailable;
                response.error = "gateway has no login zone";
                return response;
            }

            const auto current_session = context.sessions.session_for_connection(connection_id);
            if (current_session) {
                yuan::rpc::Bytes logout_payload;
                const auto logout = forward_to_zone ? forward_to_zone(current_session->zone_service_id,
                                                                      game_route::zone_player_leave(),
                                                                      GatewayForwardContext{current_session->gateway_session_id, current_session->connection_id},
                                                                      logout_payload)
                                                     : std::nullopt;
                
                if (!logout || logout->status != yuan::rpc::RpcStatus::ok) {
                    response.status = yuan::rpc::RpcStatus::unavailable;
                    response.error = logout ? logout->error : "gateway failed to logout current role before switching";
                    return response;
                }
                
                context.sessions.logout_session(current_session->gateway_session_id);
            }

            const auto gateway_session_id = context.sessions.reserve_connection(connection_id, zone_service_id);
            if (gateway_session_id == 0) {
                response.status = yuan::rpc::RpcStatus::internal_error;
                response.error = "gateway failed to allocate session";
                return response;
            }

            const auto login = forward_to_zone ? forward_to_zone(zone_service_id, game_route::zone_player_enter(), GatewayForwardContext{gateway_session_id, connection_id}, message.payload) : std::nullopt;
            if (!login || login->status != yuan::rpc::RpcStatus::ok) {
                context.sessions.logout_session(gateway_session_id);
                response.status = yuan::rpc::RpcStatus::internal_error;
                response.error = login ? login->error : "gateway login failed";
                return response;
            }

            const auto actual_zone_service_id = static_cast<PackedGameServiceId>(metadata_u64(*login, game_metadata_key::gateway_zone_service_id));
            if (!context.sessions.bind_session(gateway_session_id, actual_zone_service_id == 0 ? zone_service_id : actual_zone_service_id)) {
                context.sessions.logout_session(gateway_session_id);
                response.status = yuan::rpc::RpcStatus::internal_error;
                response.error = "gateway failed to bind login session";
                return response;
            }

            response = *login;
            strip_gateway_internal_metadata(response);

            return response;
        });

        const bool game_registered = server.register_handler(game_route::gateway_game_forward(), [&context, forward_to_zone](const yuan::rpc::Message &message) {
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            auto session = context.sessions.session_for_connection(connection_id_from(message));
            if (!session || session->zone_service_id == 0) {
                reject_not_logged_in(response);
                return response;
            }

            const auto forwarded = forward_to_zone ? forward_to_zone(session->zone_service_id, game_route::zone_echo(), GatewayForwardContext{session->gateway_session_id, session->connection_id}, message.payload) : std::nullopt;
            if (!forwarded) {
                response.status = yuan::rpc::RpcStatus::unavailable;
                response.error = "zone game request failed";
                return response;
            }

            response = *forwarded;
            strip_gateway_internal_metadata(response);

            return response;
        });

        const bool logout_registered = server.register_handler(game_route::gateway_logout(), [&context, forward_to_zone](const yuan::rpc::Message &message) {
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            auto session = context.sessions.session_for_connection(connection_id_from(message));
            if (!session || session->zone_service_id == 0) {
                reject_not_logged_in(response);
                return response;
            }

            const auto logout = forward_to_zone ? forward_to_zone(session->zone_service_id, game_route::zone_player_leave(), GatewayForwardContext{session->gateway_session_id, session->connection_id}, message.payload) : std::nullopt;
            if (!logout || logout->status != yuan::rpc::RpcStatus::ok) {
                response.status = yuan::rpc::RpcStatus::unavailable;
                response.error = logout ? logout->error : "zone logout failed";
                return response;
            }

            context.sessions.logout_session(session->gateway_session_id);
            response = *logout;
            strip_gateway_internal_metadata(response);

            return response;
        });

        const bool time_registered = server.register_handler(game_route::gateway_time_sync(), [&context, forward_to_zone](const yuan::rpc::Message &message) {
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            auto session = context.sessions.session_for_connection(connection_id_from(message));
            if (!session || session->zone_service_id == 0) {
                reject_not_logged_in(response);
                return response;
            }

            const auto forwarded = forward_to_zone ? forward_to_zone(session->zone_service_id, game_route::zone_time_sync(), GatewayForwardContext{session->gateway_session_id, session->connection_id}, message.payload) : std::nullopt;
            if (!forwarded) {
                response.status = yuan::rpc::RpcStatus::unavailable;
                response.error = "zone time sync failed";
                return response;
            }

            response = *forwarded;
            strip_gateway_internal_metadata(response);

            return response;
        });

        const bool push_registered = server.register_handler(game_route::gateway_push(), [&context, push_to_client](const yuan::rpc::Message &message) {
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            if (!has_internal_secret(message, context.gateway_internal_secret)) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "gateway internal route permission denied";
                return response;
            }
            const auto gateway_session_id = metadata_u64(message, game_metadata_key::gateway_session_id);
            if (gateway_session_id == 0) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid gateway push target";
                return response;
            }

            const auto session = context.sessions.session_info(gateway_session_id);
            if (!session || session->connection_id == 0) {
                response.status = yuan::rpc::RpcStatus::not_found;
                response.error = "gateway push target session is not logged in";
                return response;
            }

            if (!push_to_client || !push_to_client(gateway_session_id, message.payload)) {
                response.status = yuan::rpc::RpcStatus::unavailable;
                response.error = "gateway push handler failed";
                return response;
            }

            response.status = yuan::rpc::RpcStatus::ok;

            return response;
        });

        const bool close_registered = server.register_handler(game_route::gateway_session_close(), [&context, close_session](const yuan::rpc::Message &message) {
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            if (!has_internal_secret(message, context.gateway_internal_secret)) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "gateway internal route permission denied";
                return response;
            }
            const auto gateway_session_id = metadata_u64(message, game_metadata_key::gateway_session_id);
            if (gateway_session_id == 0) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid gateway session close target";
                return response;
            }
            if (!close_session || !close_session(gateway_session_id)) {
                response.status = yuan::rpc::RpcStatus::not_found;
                response.error = "gateway session close target is not logged in";
                return response;
            }
            context.sessions.logout_session(gateway_session_id);
            response.status = yuan::rpc::RpcStatus::ok;
            return response;
        });

        return login_registered && game_registered && logout_registered && time_registered && push_registered && close_registered;
    }
}
