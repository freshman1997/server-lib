#ifndef YUAN_GAME_SERVER_GLOBAL_RPC_GLOBAL_MSG_ECHO_H
#define YUAN_GAME_SERVER_GLOBAL_RPC_GLOBAL_MSG_ECHO_H

#include "common/service_node.h"

#include <cstdint>
#include <functional>

namespace yuan::game::server
{
    struct GlobalMsgEchoContext
    {
        ServiceAddress address;
        std::uint64_t request_count = 0;
        std::function<void(const yuan::rpc::Message &)> after_echo;
    };

    bool register_global_msg_echo(yuan::rpc::Server &server, GlobalMsgEchoContext &context);
}

#endif
