#ifndef __YUAN_NET_ASYNC_IOCP_SHARDED_ASYNC_LISTENER_H__
#define __YUAN_NET_ASYNC_IOCP_SHARDED_ASYNC_LISTENER_H__

#include "coroutine/task.h"
#include "net/async/async_connection_context.h"
#include "net/iocp/iocp_tcp_engine.h"
#include "net/runtime/network_runtime.h"

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
    class IocpShardedAsyncListener
    {
    public:
        using AsyncConnectionHandler = std::function<coroutine::Task<void>(AsyncConnectionContext)>;

        IocpShardedAsyncListener() = default;
        ~IocpShardedAsyncListener();

        IocpShardedAsyncListener(const IocpShardedAsyncListener &) = delete;
        IocpShardedAsyncListener &operator=(const IocpShardedAsyncListener &) = delete;

        bool listen(const std::string &host,
                    uint16_t port,
                    std::size_t shard_count,
                    std::size_t iocp_worker_count,
                    AsyncConnectionHandler handler,
                    int backlog = 128);

        void close();

        bool running() const noexcept;
        uint16_t local_port() const noexcept;
        std::size_t shard_count() const noexcept;

    private:
        struct Shard
        {
            std::unique_ptr<NetworkRuntime> runtime;
            std::thread thread;
        };

        bool start_shards(std::size_t count);
        void stop_shards();
        NetworkRuntime *next_runtime();

        IocpTcpEngine engine_;
        std::vector<Shard> shards_;
        AsyncConnectionHandler handler_;
        std::atomic_size_t next_shard_{0};
        std::atomic_bool running_{false};
    };
}

#endif
