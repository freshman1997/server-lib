#include "zone/rpc/zone_msg_gm.h"

#include <utility>

namespace yuan::game::server
{
    bool register_zone_msg_gm(yuan::rpc::Server &server, ZoneMsgGmHandler handler)
    {
        return server.register_handler(game_route::zone_gm_execute(), [handler = std::move(handler)](const yuan::rpc::Message &message) {
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            const auto request = decode_binary<SSGmCommandRequest>(message.payload);
            if (!request) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid zone gm request";
                return response;
            }
            const auto result = handler ? handler(*request) : SSGmCommandResponse{false, "zone gm handler is not configured"};
            response.status = result.ok ? yuan::rpc::RpcStatus::ok : yuan::rpc::RpcStatus::bad_request;
            (void)encode_binary(result, response.payload);
            return response;
        });
    }
}
