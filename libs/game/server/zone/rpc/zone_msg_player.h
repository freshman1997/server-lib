#ifndef YUAN_GAME_SERVER_ZONE_RPC_ZONE_MSG_PLAYER_H
#define YUAN_GAME_SERVER_ZONE_RPC_ZONE_MSG_PLAYER_H

#include "common/codec/game_binary_codec.h"

#include <functional>

namespace yuan::game::server
{
    struct ZoneMsgPlayerHandlers
    {
        std::function<bool(SSPlayerZoneUpdate)> world_zone_update;
        std::function<bool(SSGatewayLoginRequest)> player_enter;
        std::function<bool(SSGatewayLoginRequest)> player_leave;
        std::function<RoleId(std::uint64_t)> role_for_gateway_session;
        std::function<PlayerUid(RoleId)> player_uid_for_role;
    };

    bool register_zone_msg_player(yuan::rpc::Server &server,
                                  ServiceAddress address,
                                  ZoneMsgPlayerHandlers handlers);
}

#endif
