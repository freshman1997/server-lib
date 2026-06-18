#ifndef YUAN_GAME_SERVER_ZONE_RPC_ZONE_MSG_GM_H
#define YUAN_GAME_SERVER_ZONE_RPC_ZONE_MSG_GM_H

#include "common/codec/game_binary_codec.h"

#include <functional>

namespace yuan::game::server
{
    using ZoneMsgGmHandler = std::function<SSGmCommandResponse(SSGmCommandRequest)>;

    bool register_zone_msg_gm(yuan::rpc::Server &server, ZoneMsgGmHandler handler);
}

#endif
