#ifndef YUAN_GAME_SERVER_COMMON_PROTO_GM_PROTO_H
#define YUAN_GAME_SERVER_COMMON_PROTO_GM_PROTO_H

#include "common/proto/base_proto.h"

namespace yuan::game::server
{
    struct SSGmCommandRequest
    {
        PackedGameServiceId target_service_id = 0;
        std::string command;
        std::vector<std::string> args;

        YUAN_GAME_BINARY_FIELDS(target_service_id, command, args)
    };

    struct SSGmCommandResponse
    {
        bool ok = false;
        std::string message;

        YUAN_GAME_BINARY_FIELDS(ok, message)
    };
}

#endif
