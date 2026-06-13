#include "global/rpc/global_msg_echo.h"

namespace yuan::game::server
{
    bool register_global_msg_echo(yuan::rpc::Server &server, GlobalMsgEchoContext &context)
    {
        return server.register_handler(game_route::global_echo(), [&context](const yuan::rpc::Message &message) {
            context.request_count++;
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            response.status = yuan::rpc::RpcStatus::ok;
            response.payload = message.payload;
            response.metadata = message.metadata;
            response.metadata["global.node"] = service_key(context.address);
            if (context.after_echo) {
                context.after_echo(message);
            }
            return response;
        });
    }
}
