#include "gateway/gateway_service.h"

#include "base/time.h"

#include <utility>

namespace yuan::game::server
{
    GatewayService::GatewayService(ServiceAddress address)
        : ServiceNode(std::move(address))
    {
        auto now_ms = []() -> std::uint64_t {
            return yuan::base::time::system_now_sec();
        };

        (void)rpc_server().register_handler(game_route::gateway_login(), [this](const yuan::rpc::Message &message) {
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());

            const auto request = decode_client_login_request(message.payload);
            if (!request) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid gateway login request";
                return response;
            }
            if (!login_handler_) {
                response.status = yuan::rpc::RpcStatus::internal_error;
                response.error = "gateway login handler is not configured";
                return response;
            }
            const auto login = login_handler_(*request);
            if (!login) {
                response.status = yuan::rpc::RpcStatus::internal_error;
                response.error = "gateway login failed";
                return response;
            }
            response.status = yuan::rpc::RpcStatus::ok;
            (void)encode_client_login_response(*login, response.payload);
            if (login->ok && login->zone_service_id != 0) {
                zone_by_role_[login->role_id] = login->zone_service_id;
            }
            return response;
        });

        (void)rpc_server().register_handler(game_route::gateway_game_forward(), [this](const yuan::rpc::Message &message) {
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            const auto request = decode_client_game_request(message.payload);
            if (!request) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid gateway game request";
                return response;
            }
            const auto zone_it = zone_by_role_.find(request->role_id);
            if (zone_it == zone_by_role_.end()) {
                response.status = yuan::rpc::RpcStatus::not_found;
                response.error = "role is not logged in";
                return response;
            }
            if (!game_forward_handler_) {
                response.status = yuan::rpc::RpcStatus::internal_error;
                response.error = "gateway game forward handler is not configured";
                return response;
            }
            const auto forwarded = game_forward_handler_(*request, zone_it->second);
            if (!forwarded) {
                response.status = yuan::rpc::RpcStatus::unavailable;
                response.error = "zone game request failed";
                return response;
            }
            response.status = yuan::rpc::RpcStatus::ok;
            (void)encode_client_game_response(*forwarded, response.payload);
            return response;
        });

        (void)rpc_server().register_handler(game_route::gateway_logout(), [this](const yuan::rpc::Message &message) {
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            const auto request = decode_client_login_request(message.payload);
            if (!request) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid gateway logout request";
                return response;
            }
            const auto zone_it = zone_by_role_.find(request->role_id);
            if (zone_it == zone_by_role_.end()) {
                response.status = yuan::rpc::RpcStatus::not_found;
                response.error = "role is not logged in";
                return response;
            }
            if (!logout_handler_) {
                response.status = yuan::rpc::RpcStatus::internal_error;
                response.error = "gateway logout handler is not configured";
                return response;
            }
            const auto logout = logout_handler_(*request, zone_it->second);
            if (!logout) {
                response.status = yuan::rpc::RpcStatus::unavailable;
                response.error = "zone logout failed";
                return response;
            }
            if (logout->ok) {
                zone_by_role_.erase(zone_it);
            }
            response.status = yuan::rpc::RpcStatus::ok;
            (void)encode_client_login_response(*logout, response.payload);
            return response;
        });

        (void)rpc_server().register_handler(game_route::gateway_time_sync(), [this, now_ms](const yuan::rpc::Message &message) {
            const auto receive_time_seconds = now_ms();
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            const auto request = decode_client_time_sync_request(message.payload);
            if (!request) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid gateway time sync request";
                return response;
            }
            if (!zone_by_role_.contains(request->role_id)) {
                response.status = yuan::rpc::RpcStatus::not_found;
                response.error = "role is not logged in";
                return response;
            }
            response.status = yuan::rpc::RpcStatus::ok;
            (void)encode_client_time_sync_response(
                ClientTimeSyncResponse{true, request->role_id, request->client_time_seconds, receive_time_seconds, now_ms(), "time sync ok"},
                response.payload);
            return response;
        });
    }

    void GatewayService::set_login_handler(LoginHandler handler)
    {
        login_handler_ = std::move(handler);
    }

    void GatewayService::set_game_forward_handler(GameForwardHandler handler)
    {
        game_forward_handler_ = std::move(handler);
    }

    void GatewayService::set_logout_handler(LogoutHandler handler)
    {
        logout_handler_ = std::move(handler);
    }

    GatewayInfo GatewayService::public_info() const
    {
        GatewayInfo info;
        info.service_id = address().service.pack();
        info.host = public_host_;
        info.port = public_port_;
        info.name = address().name;
        return info;
    }

    void GatewayService::set_public_endpoint(std::string host, std::uint16_t port)
    {
        public_host_ = std::move(host);
        public_port_ = port;
    }
}
