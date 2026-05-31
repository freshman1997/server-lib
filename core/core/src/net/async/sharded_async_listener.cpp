#include "net/async/sharded_async_listener.h"

#include "event/event_loop.h"
#include "net/acceptor/acceptor_factory.h"
#include "net/connection/connection.h"
#include "net/connection/stream_transport.h"
#include "net/handler/connection_handler.h"
#include "net/security/ssl_module.h"
#include "net/socket/socket.h"

#include <algorithm>
#include <utility>

namespace yuan::net
{
    class ShardedAsyncListener::DispatchHandler final : public ConnectionHandler
    {
    public:
        DispatchHandler(ShardedAsyncListener &owner, NetworkRuntime &runtime)
            : owner_(owner),
              runtime_(runtime)
        {
        }

        void on_connected(const std::shared_ptr<Connection> &conn) override
        {
            if (!conn || !owner_.running_.load(std::memory_order_acquire) || !owner_.handler_) {
                if (conn) {
                    conn->close();
                }
                return;
            }

            auto *loop = runtime_.event_loop();
            if (!loop) {
                conn->close();
                return;
            }

            bool uses_readiness_channel = false;
            if (auto stream = std::dynamic_pointer_cast<StreamTransport>(conn)) {
                uses_readiness_channel = stream->stream_channel() != nullptr;
            }

            conn->set_connection_handler(uses_readiness_channel ? default_handler_holder_ : nullptr);

            auto ctx = AsyncConnectionContext(conn,
                static_cast<coroutine::RuntimeView>(runtime_.runtime_view()));
            auto task = owner_.handler_(std::move(ctx));
            task.resume();
            task.detach();
        }

        void on_error(const std::shared_ptr<Connection> &conn) override
        {
            if (conn) {
                conn->close();
            }
        }

        void on_read(const std::shared_ptr<Connection> &) override
        {
        }

        void on_write(const std::shared_ptr<Connection> &) override
        {
        }

        void on_close(const std::shared_ptr<Connection> &) override
        {
        }

        void on_input_shutdown(const std::shared_ptr<Connection> &conn) override
        {
            (void)conn;
        }

    private:
        class DefaultHandler final : public ConnectionHandler
        {
        public:
            void on_connected(const std::shared_ptr<Connection> &) override
            {
            }

            void on_error(const std::shared_ptr<Connection> &conn) override
            {
                if (conn) {
                    conn->close();
                }
            }

            void on_read(const std::shared_ptr<Connection> &) override
            {
            }

            void on_write(const std::shared_ptr<Connection> &) override
            {
            }

            void on_close(const std::shared_ptr<Connection> &) override
            {
            }

            void on_input_shutdown(const std::shared_ptr<Connection> &conn) override
            {
                (void)conn;
            }
        };

        ShardedAsyncListener &owner_;
        NetworkRuntime &runtime_;
        DefaultHandler default_handler_;
        std::shared_ptr<ConnectionHandler> default_handler_holder_{ make_non_owning_handler(default_handler_) };
    };

    ShardedAsyncListener::~ShardedAsyncListener()
    {
        close();
    }

    bool ShardedAsyncListener::listen(const std::string &host,
                                      uint16_t port,
                                      std::size_t shard_count,
                                      ListenOptions options,
                                      AsyncConnectionHandler handler)
    {
        close();
        if (!handler) {
            return false;
        }

        handler_ = std::move(handler);
        const auto actual_shards = (std::max<std::size_t>)(1, shard_count);
        running_.store(true, std::memory_order_release);

        uint16_t bind_port = port;
        for (std::size_t i = 0; i < actual_shards; ++i) {
            ListenOptions shard_options = options;
            shard_options.use_iocp = false;
#ifndef _WIN32
            if (actual_shards > 1) {
                shard_options.reuse_port = true;
            }
#endif
            if (!start_one_shard(host, bind_port, shard_options, i == 0)) {
                close();
                return false;
            }
            if (bind_port == 0) {
                bind_port = local_port_;
            }
        }

        return true;
    }

    void ShardedAsyncListener::close()
    {
        running_.store(false, std::memory_order_release);
        stop_shards();
        handler_ = {};
        local_port_ = 0;
    }

    bool ShardedAsyncListener::running() const noexcept
    {
        return running_.load(std::memory_order_acquire);
    }

    uint16_t ShardedAsyncListener::local_port() const noexcept
    {
        return local_port_;
    }

    std::size_t ShardedAsyncListener::shard_count() const noexcept
    {
        return shards_.size();
    }

    bool ShardedAsyncListener::start_one_shard(const std::string &host,
                                               uint16_t port,
                                               ListenOptions options,
                                               bool first_shard)
    {
#ifdef _WIN32
        if (!first_shard) {
            return false;
        }
#endif

        Shard shard;
        shard.runtime = std::make_unique<NetworkRuntime>();
        auto socket = std::make_unique<Socket>(host.c_str(), port);
        if (!socket->valid()) {
            return false;
        }
        if (!socket->apply_listen_options(options)) {
            return false;
        }
        if (!socket->bind()) {
            return false;
        }
        if (first_shard) {
            local_port_ = socket->get_local_address().get_port();
        }

        shard.acceptor.reset(create_stream_acceptor(socket.release()));
        if (!shard.acceptor) {
            return false;
        }
        if (!shard.acceptor->listen(options.backlog)) {
            shard.acceptor.reset();
            return false;
        }

        shard.dispatch_handler = std::make_shared<DispatchHandler>(*this, *shard.runtime);
        shard.acceptor->set_queue_pending_connections(false);
        shard.acceptor->set_connection_handler(shard.dispatch_handler);
        shard.acceptor->set_event_handler(shard.runtime->event_loop());
        if (auto *channel = shard.acceptor->listener_channel()) {
            shard.runtime->event_loop()->update_channel(channel);
        }

        auto *runtime = shard.runtime.get();
        shard.thread = std::thread([runtime]() {
            runtime->run();
        });
        shards_.push_back(std::move(shard));
        return true;
    }

    void ShardedAsyncListener::stop_shards()
    {
        for (auto &shard : shards_) {
            if (shard.acceptor) {
                shard.acceptor->close();
            }
        }

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
}
