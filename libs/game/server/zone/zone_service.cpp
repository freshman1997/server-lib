#include "zone/zone_service.h"

#include <string>
#include <utility>

namespace yuan::game::server
{
    ZoneService::ZoneService(ServiceAddress address, ForwardHandler forward_handler)
        : ServiceNode(std::move(address)), forward_handler_(std::move(forward_handler))
    {
        (void)rpc_server().register_handler(game_route::zone_echo(), [this](const yuan::rpc::Message &message) {
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            response.status = yuan::rpc::RpcStatus::ok;
            response.payload = message.payload;
            response.metadata = message.metadata;
            response.metadata["zone.node"] = service_key(this->address());
            return response;
        });

        (void)rpc_server().register_handler(game_route::zone_player_enter(), [this](const yuan::rpc::Message &message) {
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            const auto login = decode_client_login_request(message.payload);
            if (!login) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid zone login request";
                return response;
            }
            if (player_enter_handler_ && !player_enter_handler_(*login)) {
                response.status = yuan::rpc::RpcStatus::internal_error;
                response.error = "zone failed to load player data";
                return response;
            }
            const PlayerZoneUpdate update{login->role_id, this->address().service.pack()};
            if (!world_zone_update_handler_ || !world_zone_update_handler_(update)) {
                response.status = yuan::rpc::RpcStatus::internal_error;
                response.error = "world did not confirm zone login";
                return response;
            }
            response.status = yuan::rpc::RpcStatus::ok;
            (void)encode_client_login_response(ClientLoginResponse{true, login->role_id, this->address().service.pack(), "zone login ok"}, response.payload);
            return response;
        });

        (void)rpc_server().register_handler(game_route::zone_player_leave(), [this](const yuan::rpc::Message &message) {
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            const auto logout = decode_client_login_request(message.payload);
            if (!logout) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid zone logout request";
                return response;
            }
            if (player_leave_handler_ && !player_leave_handler_(*logout)) {
                response.status = yuan::rpc::RpcStatus::internal_error;
                response.error = "zone failed to unload player data";
                return response;
            }
            const PlayerZoneUpdate update{logout->role_id, 0};
            if (!world_zone_update_handler_ || !world_zone_update_handler_(update)) {
                response.status = yuan::rpc::RpcStatus::internal_error;
                response.error = "world did not confirm zone logout";
                return response;
            }
            response.status = yuan::rpc::RpcStatus::ok;
            (void)encode_client_login_response(ClientLoginResponse{true, logout->role_id, 0, "zone logout ok"}, response.payload);
            return response;
        });
    }

    yuan::rpc::Response ZoneService::call(ServiceAddress target,
                                          yuan::rpc::Route route_value,
                                          yuan::rpc::Bytes payload,
                                          yuan::rpc::Metadata metadata,
                                          yuan::rpc::RequestId request_id,
                                          yuan::rpc::ContinuationId continuation_id)
    {
        TunnelEnvelope envelope;
        envelope.source = service_key(address());
        envelope.target = service_key(target);
        envelope.source_service_id = address().service.pack();
        envelope.target_service_id = target.service.pack();
        envelope.request_id = request_id;
        envelope.continuation_id = continuation_id;
        envelope.route = std::move(route_value);
        envelope.payload = std::move(payload);
        envelope.metadata = std::move(metadata);
        if (!forward_handler_) {
            yuan::rpc::Response response;
            response.status = yuan::rpc::RpcStatus::unavailable;
            response.error = "zone forward handler is not configured";
            return response;
        }
        return forward_handler_(std::move(envelope));
    }

    void ZoneService::set_world_zone_update_handler(std::function<bool(PlayerZoneUpdate)> handler)
    {
        world_zone_update_handler_ = std::move(handler);
    }

    void ZoneService::set_player_enter_handler(std::function<bool(ClientLoginRequest)> handler)
    {
        player_enter_handler_ = std::move(handler);
    }

    void ZoneService::set_player_leave_handler(std::function<bool(ClientLoginRequest)> handler)
    {
        player_leave_handler_ = std::move(handler);
    }
}
