#ifndef YUAN_GAME_SERVER_ZONE_ZONE_SERVICE_H
#define YUAN_GAME_SERVER_ZONE_ZONE_SERVICE_H

#include "common/game_messages.h"
#include "messaging/tunnel_messages.h"

namespace yuan::game::server
{
    class ZoneService : public ServiceNode
    {
    public:
        using ForwardHandler = std::function<yuan::rpc::Response(TunnelEnvelope)>;

        ZoneService(ServiceAddress address, ForwardHandler forward_handler = {});

        yuan::rpc::Response call(ServiceAddress target,
                                 yuan::rpc::Route route,
                                 yuan::rpc::Bytes payload,
                                 yuan::rpc::Metadata metadata = {},
                                 yuan::rpc::RequestId request_id = 0,
                                 yuan::rpc::ContinuationId continuation_id = 0);

        void set_world_zone_update_handler(std::function<bool(PlayerZoneUpdate)> handler);
        void set_player_enter_handler(std::function<bool(ClientLoginRequest)> handler);
        void set_player_leave_handler(std::function<bool(ClientLoginRequest)> handler);

    private:
        ForwardHandler forward_handler_;
        std::function<bool(PlayerZoneUpdate)> world_zone_update_handler_;
        std::function<bool(ClientLoginRequest)> player_enter_handler_;
        std::function<bool(ClientLoginRequest)> player_leave_handler_;
    };
}

#endif
