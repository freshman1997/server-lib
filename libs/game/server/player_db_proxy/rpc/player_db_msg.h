#ifndef YUAN_GAME_SERVER_PLAYER_DB_PROXY_RPC_PLAYER_DB_MSG_H
#define YUAN_GAME_SERVER_PLAYER_DB_PROXY_RPC_PLAYER_DB_MSG_H

#include "common/codec/game_binary_codec.h"
#include "common/proto/player_db_proto.h"
#include "common/service_node.h"

#include "redis_client.h"

#include <memory>

namespace yuan::game::server
{
    struct PlayerDbMsgContext
    {
        ServiceAddress address;
        std::shared_ptr<yuan::redis::RedisClient> redis;
    };

    bool register_player_db_msg(yuan::rpc::Server &server, PlayerDbMsgContext &context);
}

#endif
