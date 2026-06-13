#ifndef YUAN_GAME_SERVER_ZONE_RPC_ZONE_MSG_GM_H
#define YUAN_GAME_SERVER_ZONE_RPC_ZONE_MSG_GM_H

#include "common/game_messages.h"

#include <functional>

namespace yuan::game::server
{
    using ZoneMsgGmHandler = std::function<GmCommandResponse(GmCommandRequest)>;

    bool register_zone_msg_gm(yuan::rpc::Server &server, ZoneMsgGmHandler handler);
}

#endif
