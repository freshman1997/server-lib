#include "net/async/iocp_sharded_async_listener.h"

#include <algorithm>
#include <utility>

namespace yuan::net
{
    IocpShardedAsyncListener::~IocpShardedAsyncListener()
    {
        close();
    }

    bool IocpShardedAsyncListener::listen(const std::string &host,
                                          uint16_t port,
                                          std::size_t shard_count,
                                          std::size_t iocp_worker_count,
                                          std::size_t completion_batch_size,
                                          AsyncConnectionHandler handler,
                                          int backlog)
    {
#ifdef _WIN32
        close();
        if (!handler) {
            return false;
        }

        handler_ = std::move(handler);
        const auto actual_shards = (std::max<std::size_t>)(1, shard_count);
        if (!start_shards(actual_shards)) {
            handler_ = {};
            return false;
        }
        running_.store(true, std::memory_order_release);

        IocpTcpEngineCallbacks callbacks;
        callbacks.on_accept = [this](const std::shared_ptr<IocpTcpConnection> &connection) {
            if (!connection) {
                return;
            }

            auto *runtime = next_runtime();
            if (!runtime || !handler_) {
                connection->close();
                return;
            }

            runtime->dispatch([this, connection, runtime]() {
                if (!running_.load(std::memory_order_acquire) || !handler_) {
                    connection->close();
                    return;
                }

                connection->set_event_handler(runtime->event_loop());
                AsyncConnectionContext ctx(connection,
                    static_cast<coroutine::RuntimeView>(runtime->runtime_view()));
                auto task = handler_(std::move(ctx));
                task.resume();
                task.detach();
            });
        };

        const auto actual_iocp_workers = (std::max<std::size_t>)(1, iocp_worker_count);
        if (!engine_.listen(host,
                            port,
                            actual_iocp_workers,
                            std::move(callbacks),
                            0,
                            backlog,
                            completion_batch_size)) {
            close();
            return false;
        }

        return true;
#else
        (void)host;
        (void)port;
        (void)shard_count;
        (void)iocp_worker_count;
        (void)completion_batch_size;
        (void)handler;
        (void)backlog;
        return false;
#endif
    }

    void IocpShardedAsyncListener::close()
    {
        running_.store(false, std::memory_order_release);
        engine_.stop();
        stop_shards();
        handler_ = {};
        next_shard_.store(0, std::memory_order_release);
    }

    bool IocpShardedAsyncListener::running() const noexcept
    {
        return running_.load(std::memory_order_acquire);
    }

    uint16_t IocpShardedAsyncListener::local_port() const noexcept
    {
        return engine_.local_port();
    }

    std::size_t IocpShardedAsyncListener::shard_count() const noexcept
    {
        return shards_.size();
    }

    bool IocpShardedAsyncListener::start_shards(std::size_t count)
    {
        shards_.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            Shard shard;
            shard.runtime = std::make_unique<NetworkRuntime>();
            auto *runtime = shard.runtime.get();
            shard.thread = std::thread([runtime]() {
                runtime->run();
            });
            shards_.push_back(std::move(shard));
        }
        return !shards_.empty();
    }

    void IocpShardedAsyncListener::stop_shards()
    {
        for (auto &shard : shards_) {
            if (shard.runtime) {
                shard.runtime->stop();
            }
        }

        for (auto &shard : shards_) {
            if (shard.thread.joinable()) {
                shard.thread.join();
            }
        }
        shards_.clear();
    }

    NetworkRuntime *IocpShardedAsyncListener::next_runtime()
    {
        if (shards_.empty()) {
            return nullptr;
        }

        const auto index = next_shard_.fetch_add(1, std::memory_order_relaxed) % shards_.size();
        return shards_[index].runtime.get();
    }
}
