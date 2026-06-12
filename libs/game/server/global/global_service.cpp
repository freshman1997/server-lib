#include "global/global_service.h"

#include <string>
#include <utility>

namespace yuan::game::server
{
    GlobalService::GlobalService(ServiceAddress address)
        : ServiceNode(std::move(address))
    {
        yuan::rpc::Route rpc_route;
        rpc_route.name = std::string(route::global_echo);
        (void)rpc_server().register_handler(rpc_route, [this](const yuan::rpc::Message &message) {
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
    }

    std::uint64_t GlobalService::request_count() const
    {
        return request_count_;
    }

    void GlobalService::set_after_echo(std::function<void(const yuan::rpc::Message &)> callback)
    {
        after_echo_ = std::move(callback);
    }
}
