#ifndef YUAN_GAME_SERVER_RANK_RPC_RANK_MSG_H
#define YUAN_GAME_SERVER_RANK_RPC_RANK_MSG_H

#include "common/codec/game_binary_codec.h"
#include "common/proto/rank_proto.h"
#include "common/service_node.h"

#include "redis_client.h"

#include <memory>

namespace yuan::game::server
{
    struct RankMsgContext
    {
        ServiceAddress address;
        std::shared_ptr<yuan::redis::RedisClient> redis;
    };

    bool register_rank_msg(yuan::rpc::Server &server, RankMsgContext &context);
}

#endif
