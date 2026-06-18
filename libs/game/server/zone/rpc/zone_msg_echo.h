#ifndef YUAN_GAME_SERVER_ZONE_RPC_ZONE_MSG_ECHO_H
#define YUAN_GAME_SERVER_ZONE_RPC_ZONE_MSG_ECHO_H

#include "common/codec/game_binary_codec.h"
#include "common/service_node.h"

namespace yuan::game::server
{
    bool register_zone_msg_echo(yuan::rpc::Server &server, ServiceAddress address);
}

#endif
