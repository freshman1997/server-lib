#include "global/global_service.h"

#include "base/time.h"
#include "common/gm_command_registry.h"

#include <string>
#include <utility>

namespace yuan::game::server
{
    GlobalService::GlobalService(ServiceAddress address)
        : ServiceNode(std::move(address))
    {
        (void)rpc_server().register_handler(game_route::global_echo(), [this](const yuan::rpc::Message &message) {
            request_count_++;
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            response.status = yuan::rpc::RpcStatus::ok;
            response.payload = message.payload;
            response.metadata = message.metadata;
            response.metadata["global.node"] = service_key(this->address());
            if (after_echo_) {
                after_echo_(message);
            }
            return response;
        });

        register_builtin_gm_commands();
        register_gm_command("set_time_offset_seconds", [](const std::vector<std::string> &args) {
            if (args.size() != 1) {
                return GmCommandResponse{false, "usage: set_time_offset_seconds <seconds>"};
            }
            const auto offset_seconds = static_cast<std::int64_t>(std::stoll(args.front()));
            yuan::base::time::set_system_time_offset_seconds(offset_seconds);
            return GmCommandResponse{true, "time offset seconds set to " + std::to_string(offset_seconds)};
        });

        (void)rpc_server().register_handler(game_route::global_gm_execute(), [this](const yuan::rpc::Message &message) {
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            const auto request = decode_gm_command_request(message.payload);
            if (!request) {
                response.status = yuan::rpc::RpcStatus::bad_request;
                response.error = "invalid gm command request";
                return response;
            }
            const auto it = gm_executors_.find(request->command);
            if (it == gm_executors_.end()) {
                response.status = yuan::rpc::RpcStatus::not_found;
                (void)encode_gm_command_response(GmCommandResponse{false, "unknown gm command: " + request->command}, response.payload);
                return response;
            }
            const auto result = it->second(request->args);
            response.status = result.ok ? yuan::rpc::RpcStatus::ok : yuan::rpc::RpcStatus::bad_request;
            (void)encode_gm_command_response(result, response.payload);
            return response;
        });
    }

    std::uint64_t GlobalService::request_count() const
    {
        return request_count_;
    }

    void GlobalService::set_after_echo(std::function<void(const yuan::rpc::Message &)> callback)
    {
        after_echo_ = std::move(callback);
    }

    void GlobalService::register_gm_command(std::string command, std::function<GmCommandResponse(const std::vector<std::string> &)> executor)
    {
        gm_executors_[std::move(command)] = std::move(executor);
    }
}
