#include "gateway/rpc/gateway_msg_client.h"

#include "base/time.h"
#include "common/rpc_network.h"

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

        void close_after_response(yuan::rpc::Response &response)
        {
            response.metadata[rpc_network::metadata_key::close_connection] = "1";
        }
    }

    GatewayInfo gateway_public_info(const GatewayMsgContext &context)
    {
        GatewayInfo info;
        info.service_id = context.address.service.pack();
        info.host = context.public_host;
        info.port = context.public_port;
        info.name = context.address.name;
        return info;
    }

    bool register_gateway_msg_client(yuan::rpc::Server &server, GatewayMsgContext &context)
    {
        const bool login_registered = server.register_handler(game_route::gateway_login(), [&context](const yuan::rpc::Message &message) {
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());

            const auto frame = decode_client_frame(message.payload);
            const auto request = decode_client_login_request(frame ? frame->payload : message.payload);
            if (!request) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid gateway login request";
                close_after_response(response);
                return response;
            }
            if (frame && (frame->header.route_service != message.route.service || frame->header.route_method != message.route.method ||
                          frame->header.player_uid != request->player_uid || frame->header.role_id != request->role_id ||
                          frame->header.zone_service_id != request->zone_service_id || frame->header.gateway_session_id != request->gateway_session_id)) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "gateway login frame header mismatch";
                close_after_response(response);
                return response;
            }
            if (!context.login_handler) {
                response.status = yuan::rpc::RpcStatus::internal_error;
                response.error = "gateway login handler is not configured";
                return response;
            }
            auto login_request = *request;
            if (login_request.zone_service_id != 0) {
                login_request.gateway_session_id = context.sessions.login_role(login_request.role_id,
                                                                              login_request.zone_service_id,
                                                                              connection_id_from(message));
            }
            const auto login = context.login_handler(login_request);
            if (!login) {
                if (login_request.gateway_session_id != 0) {
                    context.sessions.logout_role(login_request.role_id);
                }
                response.status = yuan::rpc::RpcStatus::internal_error;
                response.error = "gateway login failed";
                return response;
            }
            response.status = yuan::rpc::RpcStatus::ok;
            auto login_result = *login;
            if (login->ok && login->zone_service_id != 0) {
                login_result.gateway_session_id = login_request.gateway_session_id;
            } else if (login_request.gateway_session_id != 0) {
                context.sessions.logout_role(login_request.role_id);
            }
            (void)encode_client_login_response(login_result, response.payload);
            return response;
        });

        const bool game_registered = server.register_handler(game_route::gateway_game_forward(), [&context](const yuan::rpc::Message &message) {
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            const auto frame = decode_client_frame(message.payload);
            const auto request = decode_client_game_request(frame ? frame->payload : message.payload);
            if (!request) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid gateway game request";
                close_after_response(response);
                return response;
            }
            if (frame && (frame->header.route_service != message.route.service || frame->header.route_method != message.route.method ||
                          frame->header.player_uid != request->player_uid || frame->header.role_id != request->role_id ||
                          frame->header.gateway_session_id != request->gateway_session_id)) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "gateway game frame header mismatch";
                close_after_response(response);
                return response;
            }
            if (frame) {
                const auto validation = context.frame_replay_guard.validate(*frame, context.frame_validation_options);
                if (!validation.ok) {
                    response.status = yuan::rpc::RpcStatus::bad_request;
                    response.error = validation.error;
                    close_after_response(response);
                    return response;
                }
            }
            const auto zone_service_id = context.sessions.zone_for_role(request->role_id);
            if (zone_service_id == 0 || !context.sessions.validate_role_session(request->role_id, request->gateway_session_id)) {
                response.status = yuan::rpc::RpcStatus::not_found;
                response.error = "role is not logged in";
                close_after_response(response);
                return response;
            }
            if (!context.game_forward_handler) {
                response.status = yuan::rpc::RpcStatus::internal_error;
                response.error = "gateway game forward handler is not configured";
                return response;
            }
            const auto forwarded = context.game_forward_handler(*request, zone_service_id);
            if (!forwarded) {
                response.status = yuan::rpc::RpcStatus::unavailable;
                response.error = "zone game request failed";
                return response;
            }
            response.status = yuan::rpc::RpcStatus::ok;
            (void)encode_client_game_response(*forwarded, response.payload);
            return response;
        });

        const bool logout_registered = server.register_handler(game_route::gateway_logout(), [&context](const yuan::rpc::Message &message) {
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            const auto frame = decode_client_frame(message.payload);
            const auto request = decode_client_login_request(frame ? frame->payload : message.payload);
            if (!request) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid gateway logout request";
                close_after_response(response);
                return response;
            }
            if (frame && (frame->header.route_service != message.route.service || frame->header.route_method != message.route.method ||
                          frame->header.player_uid != request->player_uid || frame->header.role_id != request->role_id ||
                          frame->header.gateway_session_id != request->gateway_session_id)) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "gateway logout frame header mismatch";
                close_after_response(response);
                return response;
            }
            if (frame) {
                const auto validation = context.frame_replay_guard.validate(*frame, context.frame_validation_options);
                if (!validation.ok) {
                    response.status = yuan::rpc::RpcStatus::bad_request;
                    response.error = validation.error;
                    close_after_response(response);
                    return response;
                }
            }
            const auto zone_service_id = context.sessions.zone_for_role(request->role_id);
            if (zone_service_id == 0 || !context.sessions.validate_role_session(request->role_id, request->gateway_session_id)) {
                response.status = yuan::rpc::RpcStatus::not_found;
                response.error = "role is not logged in";
                close_after_response(response);
                return response;
            }
            if (!context.logout_handler) {
                response.status = yuan::rpc::RpcStatus::internal_error;
                response.error = "gateway logout handler is not configured";
                return response;
            }
            const auto logout = context.logout_handler(*request, zone_service_id);
            if (!logout) {
                response.status = yuan::rpc::RpcStatus::unavailable;
                response.error = "zone logout failed";
                return response;
            }
            if (logout->ok) {
                context.sessions.logout_session(request->gateway_session_id);
                context.frame_replay_guard.erase_session(request->gateway_session_id);
            }
            response.status = yuan::rpc::RpcStatus::ok;
            (void)encode_client_login_response(*logout, response.payload);
            return response;
        });

        const bool time_registered = server.register_handler(game_route::gateway_time_sync(), [&context](const yuan::rpc::Message &message) {
            const auto receive_time_seconds = yuan::base::time::system_now_sec();
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            const auto frame = decode_client_frame(message.payload);
            const auto request = decode_client_time_sync_request(frame ? frame->payload : message.payload);
            if (!request) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid gateway time sync request";
                close_after_response(response);
                return response;
            }
            if (frame && (frame->header.route_service != message.route.service || frame->header.route_method != message.route.method ||
                          frame->header.player_uid != request->player_uid || frame->header.role_id != request->role_id ||
                          frame->header.gateway_session_id != request->gateway_session_id)) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "gateway time sync frame header mismatch";
                close_after_response(response);
                return response;
            }
            if (frame) {
                const auto validation = context.frame_replay_guard.validate(*frame, context.frame_validation_options);
                if (!validation.ok) {
                    response.status = yuan::rpc::RpcStatus::bad_request;
                    response.error = validation.error;
                    close_after_response(response);
                    return response;
                }
            }
            if (!context.sessions.validate_role_session(request->role_id, request->gateway_session_id)) {
                response.status = yuan::rpc::RpcStatus::not_found;
                response.error = "role is not logged in";
                close_after_response(response);
                return response;
            }
            response.status = yuan::rpc::RpcStatus::ok;
            (void)encode_client_time_sync_response(
                ClientTimeSyncResponse{true, request->role_id, request->client_time_seconds, receive_time_seconds, yuan::base::time::system_now_sec(), "time sync ok"},
                response.payload);
            return response;
        });

        const bool push_registered = server.register_handler(game_route::gateway_push(), [&context](const yuan::rpc::Message &message) {
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            const auto push = decode_client_push_message(message.payload);
            if (!push) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid gateway push message";
                return response;
            }
            if (!context.sessions.validate_role_session(push->role_id, push->gateway_session_id)) {
                response.status = yuan::rpc::RpcStatus::not_found;
                response.error = "gateway push target session is not logged in";
                return response;
            }
            if (context.push_handler && !context.push_handler(*push)) {
                response.status = yuan::rpc::RpcStatus::unavailable;
                response.error = "gateway push handler failed";
                return response;
            }
            response.status = yuan::rpc::RpcStatus::ok;
            return response;
        });

        return login_registered && game_registered && logout_registered && time_registered && push_registered;
    }
}
