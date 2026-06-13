#include "zone/rpc/zone_msg_echo.h"

namespace yuan::game::server
{
    bool register_zone_msg_echo(yuan::rpc::Server &server, ServiceAddress address)
    {
        return server.register_handler(game_route::zone_echo(), [address = std::move(address)](const yuan::rpc::Message &message) {
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            response.status = yuan::rpc::RpcStatus::ok;
            response.payload = message.payload;
            response.metadata = message.metadata;
            response.metadata["zone.node"] = service_key(address);
            return response;
        });
    }
}
