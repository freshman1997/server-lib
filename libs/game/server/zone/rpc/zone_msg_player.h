#ifndef YUAN_GAME_SERVER_ZONE_RPC_ZONE_MSG_PLAYER_H
#define YUAN_GAME_SERVER_ZONE_RPC_ZONE_MSG_PLAYER_H

#include "common/game_messages.h"

#include <functional>

namespace yuan::game::server
{
    struct ZoneMsgPlayerHandlers
    {
        std::function<bool(PlayerZoneUpdate)> world_zone_update;
        std::function<bool(ClientLoginRequest)> player_enter;
        std::function<bool(ClientLoginRequest)> player_leave;
    };

    bool register_zone_msg_player(yuan::rpc::Server &server,
                                  ServiceAddress address,
                                  ZoneMsgPlayerHandlers handlers);
}

#endif
