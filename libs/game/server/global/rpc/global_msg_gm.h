#ifndef YUAN_GAME_SERVER_GLOBAL_RPC_GLOBAL_MSG_GM_H
#define YUAN_GAME_SERVER_GLOBAL_RPC_GLOBAL_MSG_GM_H

#include "common/game_messages.h"

#include <functional>
#include <string>
#include <unordered_map>

namespace yuan::game::server
{
    struct GlobalMsgGmContext
    {
        std::unordered_map<std::string, std::function<GmCommandResponse(const std::vector<std::string> &)>> executors;
    };

    void register_global_builtin_gm(GlobalMsgGmContext &context);
    bool register_global_msg_gm(yuan::rpc::Server &server, GlobalMsgGmContext &context);
}

#endif
