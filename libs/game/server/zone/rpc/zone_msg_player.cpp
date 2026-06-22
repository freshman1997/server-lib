#include "zone/rpc/zone_msg_player.h"

#include "base/time.h"
#include "common/metadata_keys.h"

#include <cstdlib>
#include <functional>
#include <utility>

namespace yuan::game::server
{
    namespace
    {
        std::uint64_t metadata_u64(const yuan::rpc::Message &message, const char *key)
        {
            const auto it = message.metadata.find(key);
            if (it == message.metadata.end()) {
                return 0;
            }
            return static_cast<std::uint64_t>(std::strtoull(it->second.c_str(), nullptr, 10));
        }

        yuan::rpc::Response make_response_for(const yuan::rpc::Message &message)
        {
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            response.metadata = message.metadata;
            return response;
        }

        yuan::rpc::Response handle_zone_player_enter(ServiceAddress address, const ZoneMsgPlayerHandlers &handlers, const yuan::rpc::Message &message)
        {
            auto response = make_response_for(message);
            const auto client_login = decode_binary<CSLoginRequest>(message.payload);
            const SSGatewayLoginRequest login{client_login ? client_login->player_uid : 0,
                                              client_login ? client_login->role_id : 0,
                                              address.service.pack(),
                                              metadata_u64(message, game_metadata_key::gateway_session_id)};
            if (!client_login || login.player_uid == 0 || login.role_id == 0 || login.gateway_session_id == 0) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid zone login request";
                return response;
            }

            if (handlers.player_enter && !handlers.player_enter(login)) {
                response.status = yuan::rpc::RpcStatus::unavailable;
                response.error = "zone admission rejected player login";
                return response;
            }

            const SSPlayerZoneUpdate update{login.player_uid, login.role_id, address.service.pack(), address.service.pack(), login.gateway_session_id};
            if (!handlers.world_zone_update || !handlers.world_zone_update(update)) {
                response.status = yuan::rpc::RpcStatus::internal_error;
                response.error = "world did not confirm zone login";
                return response;
            }

            response.status = yuan::rpc::RpcStatus::ok;
            response.metadata[game_metadata_key::gateway_zone_service_id] = std::to_string(address.service.pack());
            response.metadata[game_metadata_key::gateway_session_id] = std::to_string(login.gateway_session_id);
            (void)encode_binary(CSLoginResponse{true, login.role_id, 0, 0, "zone login ok"}, response.payload);
            return response;
        }

        yuan::rpc::Response handle_zone_player_leave(ServiceAddress address, const ZoneMsgPlayerHandlers &handlers, const yuan::rpc::Message &message)
        {
            auto response = make_response_for(message);
            const auto client_logout = message.payload.empty() ? std::optional<CSLoginRequest>{} : decode_binary<CSLoginRequest>(message.payload);
            const auto gateway_session_id = metadata_u64(message, game_metadata_key::gateway_session_id);
            RoleId role_id = client_logout ? client_logout->role_id : 0;
            if (role_id == 0 && handlers.role_for_gateway_session) {
                role_id = handlers.role_for_gateway_session(gateway_session_id);
            }

            const auto player_uid = handlers.player_uid_for_role ? handlers.player_uid_for_role(role_id) : 0;
            const SSGatewayLoginRequest logout{0, role_id, address.service.pack(), gateway_session_id};
            if (logout.role_id == 0 || logout.gateway_session_id == 0) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid zone logout request";
                return response;
            }

            if (handlers.player_leave && !handlers.player_leave(logout)) {
                response.status = yuan::rpc::RpcStatus::internal_error;
                response.error = "zone failed to unload player data";
                return response;
            }

            const SSPlayerZoneUpdate update{player_uid, logout.role_id, 0, address.service.pack(), logout.gateway_session_id};
            if (!handlers.world_zone_update || !handlers.world_zone_update(update)) {
                response.status = yuan::rpc::RpcStatus::internal_error;
                response.error = "world did not confirm zone logout";
                return response;
            }

            response.status = yuan::rpc::RpcStatus::ok;
            (void)encode_binary(CSLoginResponse{true, logout.role_id, 0, 0, "zone logout ok"}, response.payload);
            return response;
        }

        yuan::rpc::Response handle_zone_time_sync(const yuan::rpc::Message &message)
        {
            const auto receive_time_seconds = yuan::base::time::system_now_sec();
            auto response = make_response_for(message);
            const auto request = decode_binary<CSTimeSyncRequest>(message.payload);
            if (!request || request->role_id == 0 || metadata_u64(message, game_metadata_key::gateway_session_id) == 0) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid zone time sync request";
                return response;
            }
            
            response.status = yuan::rpc::RpcStatus::ok;
            (void)encode_binary(CSTimeSyncResponse{true, request->role_id, request->client_time_seconds, receive_time_seconds, yuan::base::time::system_now_sec(), "time sync ok"}, response.payload);
            return response;
        }
    }

    bool register_zone_msg_player(yuan::rpc::Server &server,
                                  ServiceAddress address,
                                  ZoneMsgPlayerHandlers handlers)
    {
        const bool enter_registered = server.register_handler(game_route::zone_player_enter(), std::bind_front(handle_zone_player_enter, address, handlers));
        const bool leave_registered = server.register_handler(game_route::zone_player_leave(), std::bind_front(handle_zone_player_leave, address, std::move(handlers)));
        const bool time_registered = server.register_handler(game_route::zone_time_sync(), handle_zone_time_sync);

        return enter_registered && leave_registered && time_registered;
    }
}
