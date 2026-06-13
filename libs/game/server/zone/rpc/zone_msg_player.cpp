#include "zone/rpc/zone_msg_player.h"

#include <utility>

namespace yuan::game::server
{
    bool register_zone_msg_player(yuan::rpc::Server &server,
                                  ServiceAddress address,
                                  ZoneMsgPlayerHandlers handlers)
    {
        const bool enter_registered = server.register_handler(game_route::zone_player_enter(), [address, handlers](const yuan::rpc::Message &message) {
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            response.metadata = message.metadata;
            const auto login = decode_client_login_request(message.payload);
            if (!login) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid zone login request";
                return response;
            }
            if (handlers.player_enter && !handlers.player_enter(*login)) {
                response.status = yuan::rpc::RpcStatus::unavailable;
                response.error = "zone admission rejected player login";
                return response;
            }
            const PlayerZoneUpdate update{login->role_id, address.service.pack(), address.service.pack(), login->gateway_session_id};
            if (!handlers.world_zone_update || !handlers.world_zone_update(update)) {
                response.status = yuan::rpc::RpcStatus::internal_error;
                response.error = "world did not confirm zone login";
                return response;
            }
            response.status = yuan::rpc::RpcStatus::ok;
            (void)encode_client_login_response(ClientLoginResponse{true, login->role_id, address.service.pack(), login->gateway_session_id, "zone login ok"}, response.payload);
            return response;
        });

        const bool leave_registered = server.register_handler(game_route::zone_player_leave(), [address, handlers = std::move(handlers)](const yuan::rpc::Message &message) {
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            response.metadata = message.metadata;
            const auto logout = decode_client_login_request(message.payload);
            if (!logout) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid zone logout request";
                return response;
            }
            if (handlers.player_leave && !handlers.player_leave(*logout)) {
                response.status = yuan::rpc::RpcStatus::internal_error;
                response.error = "zone failed to unload player data";
                return response;
            }
            const PlayerZoneUpdate update{logout->role_id, 0, address.service.pack(), logout->gateway_session_id};
            if (!handlers.world_zone_update || !handlers.world_zone_update(update)) {
                response.status = yuan::rpc::RpcStatus::internal_error;
                response.error = "world did not confirm zone logout";
                return response;
            }
            response.status = yuan::rpc::RpcStatus::ok;
            (void)encode_client_login_response(ClientLoginResponse{true, logout->role_id, 0, logout->gateway_session_id, "zone logout ok"}, response.payload);
            return response;
        });

        return enter_registered && leave_registered;
    }
}
