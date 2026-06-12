#ifndef YUAN_GAME_SERVER_ZONE_ZONE_SERVICE_H
#define YUAN_GAME_SERVER_ZONE_ZONE_SERVICE_H

#include "tunnel/tunnel_service.h"

namespace yuan::game::server
{
    class ZoneService : public ServiceNode
    {
    public:
        ZoneService(ServiceAddress address, TunnelCluster &tunnels);

        yuan::rpc::Response call(ServiceAddress target,
                                 yuan::rpc::Route route,
                                 yuan::rpc::Bytes payload,
                                 yuan::rpc::Metadata metadata = {},
                                 yuan::rpc::RequestId request_id = 0,
                                 yuan::rpc::ContinuationId continuation_id = 0);

    private:
        TunnelCluster &tunnels_;
    };
}

#endif
