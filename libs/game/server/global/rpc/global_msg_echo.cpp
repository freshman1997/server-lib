#include "global/rpc/global_msg_echo.h"

#include "common/metadata_keys.h"

#include <functional>

namespace yuan::game::server
{
    namespace
    {
        yuan::rpc::Response handle_global_echo(GlobalMsgEchoContext &context, const yuan::rpc::Message &message)
        {
            context.request_count++;
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            response.status = yuan::rpc::RpcStatus::ok;
            response.payload = message.payload;
            response.metadata = message.metadata;
            response.metadata[game_metadata_key::global_node] = service_key(context.address);
            if (context.after_echo) {
                context.after_echo(message);
            }
            return response;
        }
    }

    bool register_global_msg_echo(yuan::rpc::Server &server, GlobalMsgEchoContext &context)
    {
        return server.register_handler(game_route::global_echo(), std::bind_front(handle_global_echo, std::ref(context)));
    }
}
