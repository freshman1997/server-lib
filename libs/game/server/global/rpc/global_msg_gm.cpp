#include "global/rpc/global_msg_gm.h"

#include "base/time.h"
#include "common/gm_command_registry.h"

#include <functional>
#include <string>
#include <utility>

namespace yuan::game::server
{
    namespace
    {
        SSGmCommandResponse handle_set_time_offset_seconds(const std::vector<std::string> &args)
        {
            if (args.size() != 1) {
                return SSGmCommandResponse{false, "usage: set_time_offset_seconds <seconds>"};
            }
            
            const auto offset_seconds = static_cast<std::int64_t>(std::stoll(args.front()));
            yuan::base::time::set_system_time_offset_seconds(offset_seconds);
            return SSGmCommandResponse{true, "time offset seconds set to " + std::to_string(offset_seconds)};
        }

        yuan::rpc::Response handle_global_gm_execute(GlobalMsgGmContext &context, const yuan::rpc::Message &message)
        {
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            const auto request = decode_binary<SSGmCommandRequest>(message.payload);
            if (!request) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid gm command request";
                return response;
            }

            const auto it = context.executors.find(request->command);
            if (it == context.executors.end()) {
                response.status = yuan::rpc::RpcStatus::not_found;
                (void)encode_binary(SSGmCommandResponse{false, "unknown gm command: " + request->command}, response.payload);
                return response;
            }

            const auto result = it->second(request->args);
            response.status = result.ok ? yuan::rpc::RpcStatus::ok : yuan::rpc::RpcStatus::bad_request;
            (void)encode_binary(result, response.payload);

            return response;
        }
    }

    void register_global_builtin_gm(GlobalMsgGmContext &context)
    {
        register_builtin_gm_commands();
        context.executors["set_time_offset_seconds"] = handle_set_time_offset_seconds;
    }

    bool register_global_msg_gm(yuan::rpc::Server &server, GlobalMsgGmContext &context)
    {
        return server.register_handler(game_route::global_gm_execute(), std::bind_front(handle_global_gm_execute, std::ref(context)));
    }
}
