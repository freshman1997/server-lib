#ifndef __YUAN_NET_ASYNC_ASYNC_LISTENER_HOST_H__
#define __YUAN_NET_ASYNC_ASYNC_LISTENER_HOST_H__

#include <functional>
#include <memory>
#include <string>

#include "coroutine/accept_awaitable.h"
#include "coroutine/runtime.h"
#include "coroutine/task.h"
#include "net/acceptor/acceptor_factory.h"
#include "net/acceptor/stream_acceptor.h"
#include "net/async/async_connection_context.h"
#include "net/connection/connection.h"
#include "net/connection/stream_transport.h"
#include "net/runtime/network_runtime.h"
#include "net/socket/socket.h"
#include "net/secuity/ssl_module.h"

namespace yuan::net
{

    class AsyncListenerHost
    {
    public:
        using AsyncConnectionHandler = std::function<coroutine::Task<void>(AsyncConnectionContext)>;

        AsyncListenerHost() = default;

        ~AsyncListenerHost()
        {
            close();
        }

        AsyncListenerHost(const AsyncListenerHost &) = delete;
        AsyncListenerHost &operator=(const AsyncListenerHost &) = delete;

        bool bind(uint16_t port, NetworkRuntime &runtime)
        {
            return setup("", port, runtime);
        }

        bool bind(const std::string &host, uint16_t port, NetworkRuntime &runtime)
        {
            return setup(host, port, runtime);
        }

        void close()
        {
            if (acceptor_) {
                delete acceptor_;
                acceptor_ = nullptr;
            }
            runtime_ = nullptr;
        }

        void set_ssl_module(std::shared_ptr<SSLModule> ssl_module)
        {
            ssl_module_ = std::move(ssl_module);
        }

        void set_connection_handler(AsyncConnectionHandler handler)
        {
            conn_handler_ = std::move(handler);
        }

        coroutine::Task<Connection *> accept_async()
        {
            if (!acceptor_ || !runtime_) {
                co_return nullptr;
            }

            auto rv = runtime_->runtime_view();
            auto *conn = co_await coroutine::async_accept(rv, acceptor_);
            co_return conn;
        }

        coroutine::Task<void> run_async()
        {
            auto rv = runtime_->runtime_view();
            while (true) {
                auto *conn = co_await coroutine::async_accept(rv, acceptor_);
                if (!conn) {
                    break;
                }

                conn->set_event_handler(runtime_->event_loop());
                conn->set_connection_handler(&default_handler_);

                if (auto *stream = dynamic_cast<StreamTransport *>(conn)) {
                    if (auto *channel = stream->stream_channel()) {
                        runtime_->event_loop()->update_channel(channel);
                    }
                }

                if (conn_handler_) {
                    auto ctx = AsyncConnectionContext(conn, static_cast<coroutine::RuntimeView>(rv));
                    auto task = conn_handler_(std::move(ctx));
                    task.resume();
                    task.detach();
                }
            }
        }

        bool is_listening() const noexcept
        {
            return acceptor_ != nullptr;
        }

        NetworkRuntime *runtime() const noexcept
        {
            return runtime_;
        }

        StreamAcceptor *acceptor() const noexcept
        {
            return acceptor_;
        }

    private:
        bool setup(const std::string &host, uint16_t port, NetworkRuntime &runtime)
        {
            runtime_ = &runtime;

            auto *loop = runtime.event_loop();
            auto *tm = runtime.timer_manager();
            if (!loop || !tm) {
                return false;
            }

            Socket *sock = new Socket(host.c_str(), port);
            if (!sock->valid()) {
                delete sock;
                return false;
            }

#ifdef _WIN32
            sock->set_reuse(true, true);
#else
            sock->set_reuse(true);
#endif
            sock->set_none_block(true);
            if (!sock->bind()) {
                delete sock;
                return false;
            }

            acceptor_ = create_stream_acceptor(sock);
            if (!acceptor_->listen()) {
                delete acceptor_;
                acceptor_ = nullptr;
                return false;
            }

            if (ssl_module_) {
                acceptor_->set_ssl_module(ssl_module_);
            }

            acceptor_->set_connection_handler(&default_handler_);
            acceptor_->set_event_handler(loop);
            if (auto *channel = acceptor_->listener_channel()) {
                loop->update_channel(channel);
            }
            return true;
        }

        class DefaultHandler final : public yuan::net::ConnectionHandler
        {
        public:
            void on_connected(Connection *) override
            {
            }
            void on_error(Connection *conn) override
            {
                if (conn) {
                    conn->close();
                }
            }
            void on_read(Connection *) override
            {
            }
            void on_write(Connection *) override
            {
            }
            void on_close(Connection *) override
            {
            }
        };

        NetworkRuntime *runtime_ = nullptr;
        StreamAcceptor *acceptor_ = nullptr;
        std::shared_ptr<SSLModule> ssl_module_;
        AsyncConnectionHandler conn_handler_;
        DefaultHandler default_handler_;
    };

} // namespace yuan::net

#endif
