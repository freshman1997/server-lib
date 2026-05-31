#ifndef __YUAN_NET_ASYNC_SHARDED_ASYNC_LISTENER_H__
#define __YUAN_NET_ASYNC_SHARDED_ASYNC_LISTENER_H__

#include "coroutine/task.h"
#include "net/acceptor/stream_acceptor.h"
#include "net/async/async_connection_context.h"
#include "net/handler/connection_handler.h"
#include "net/runtime/network_runtime.h"
#include "net/socket/listen_options.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace yuan::net
{
    class ShardedAsyncListener
    {
    public:
        using AsyncConnectionHandler = std::function<coroutine::Task<void>(AsyncConnectionContext)>;

        ShardedAsyncListener() = default;
        ~ShardedAsyncListener();

        ShardedAsyncListener(const ShardedAsyncListener &) = delete;
        ShardedAsyncListener &operator=(const ShardedAsyncListener &) = delete;

        bool listen(const std::string &host,
                    uint16_t port,
                    std::size_t shard_count,
                    ListenOptions options,
                    AsyncConnectionHandler handler);

        void close();

        bool running() const noexcept;
        uint16_t local_port() const noexcept;
        std::size_t shard_count() const noexcept;

    private:
        class DispatchHandler;

        struct Shard
        {
            std::unique_ptr<NetworkRuntime> runtime;
            std::unique_ptr<StreamAcceptor> acceptor;
            std::shared_ptr<DispatchHandler> dispatch_handler;
            std::thread thread;
        };

        bool start_one_shard(const std::string &host,
                             uint16_t port,
                             ListenOptions options,
                             bool first_shard);
        void stop_shards();

        std::vector<Shard> shards_;
        AsyncConnectionHandler handler_;
        std::atomic_bool running_{false};
        uint16_t local_port_ = 0;
    };
}

#endif
