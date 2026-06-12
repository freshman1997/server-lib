#include "zone/zone_service.h"

#include <string>
#include <utility>

namespace yuan::game::server
{
    ZoneService::ZoneService(ServiceAddress address, TunnelCluster &tunnels)
        : ServiceNode(std::move(address)), tunnels_(tunnels)
    {
        yuan::rpc::Route rpc_route;
        rpc_route.name = std::string(route::zone_echo);
        (void)rpc_server().register_handler(rpc_route, [this](const yuan::rpc::Message &message) {
            yuan::rpc::Response response;
            response.request_id = message.request_id;
            response.set_continuation_id(message.continuation_id());
            response.status = yuan::rpc::RpcStatus::ok;
            response.payload = message.payload;
            response.metadata = message.metadata;
            response.metadata["zone.node"] = service_key(this->address());
            return response;
        });
    }

    yuan::rpc::Response ZoneService::call(ServiceAddress target,
                                          yuan::rpc::Route route_value,
                                          yuan::rpc::Bytes payload,
                                          yuan::rpc::Metadata metadata,
                                          yuan::rpc::RequestId request_id,
                                          yuan::rpc::ContinuationId continuation_id)
    {
        TunnelEnvelope envelope;
        envelope.source = service_key(address());
        envelope.target = service_key(target);
        envelope.source_service_id = address().service.pack();
        envelope.target_service_id = target.service.pack();
        envelope.request_id = request_id;
        envelope.continuation_id = continuation_id;
        envelope.route = std::move(route_value);
        envelope.payload = std::move(payload);
        envelope.metadata = std::move(metadata);
        return tunnels_.forward(std::move(envelope));
    }
}
