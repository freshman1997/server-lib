#include "zone/rpc/zone_msg_echo.h"

#include "common/metadata_keys.h"

#include <functional>

namespace yuan::game::server
{
    namespace
    {
        yuan::rpc::Response handle_zone_echo(ServiceAddress address, const yuan::rpc::Message &message)
        {
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            response.metadata = message.metadata;
            const auto request = decode_binary<CSGameRequest>(message.payload);
            if (!request || request->role_id == 0 || message.metadata.find(game_metadata_key::gateway_session_id) == message.metadata.end()) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid zone game request";
                return response;
            }
            
            response.status = yuan::rpc::RpcStatus::ok;
            (void)encode_binary(CSGameResponse{true, request->role_id, 0, request->payload, "zone game ok"}, response.payload);
            response.metadata[game_metadata_key::zone_node] = service_key(address);
            return response;
        }
    }

    bool register_zone_msg_echo(yuan::rpc::Server &server, ServiceAddress address)
    {
        return server.register_handler(game_route::zone_echo(), std::bind_front(handle_zone_echo, std::move(address)));
    }
}
