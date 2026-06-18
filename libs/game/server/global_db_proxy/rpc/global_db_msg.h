#ifndef YUAN_GAME_SERVER_GLOBAL_DB_PROXY_RPC_GLOBAL_DB_MSG_H
#define YUAN_GAME_SERVER_GLOBAL_DB_PROXY_RPC_GLOBAL_DB_MSG_H

#include "common/codec/game_binary_codec.h"
#include "common/proto/global_db_proto.h"
#include "common/service_node.h"

#include "redis_client.h"
#include "redis_async_executor.h"
#include "redis_client_pool.h"

#include <functional>
#include <memory>

namespace yuan::game::server
{
    struct GlobalDbMsgContext
    {
        ServiceAddress address;
        std::shared_ptr<yuan::redis::RedisClient> redis;
        std::shared_ptr<yuan::redis::RedisClientPool> redis_pool;
        yuan::redis::RedisAsyncExecutor *redis_executor = nullptr;
        yuan::coroutine::RuntimeView resume_runtime;
        std::function<void(std::uint64_t, yuan::rpc::Response)> write_deferred_response;
    };

    bool register_global_db_msg(yuan::rpc::Server &server, GlobalDbMsgContext &context);
}

#endif
