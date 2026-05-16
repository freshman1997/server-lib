#ifndef __YUAN_NET_ASYNC_ASYNC_LISTENER_HOST_H__
#define __YUAN_NET_ASYNC_ASYNC_LISTENER_HOST_H__

#include <functional>
#include <memory>
#include <string>
#include <utility>

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
#include "net/security/ssl_module.h"

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
            return setup("", port, runtime, {});
        }

        bool bind(const std::string &host, uint16_t port, NetworkRuntime &runtime)
        {
            return setup(host, port, runtime, {});
        }

        bool bind(uint16_t port, NetworkRuntime &runtime, const ListenOptions &options)
        {
            return setup("", port, runtime, options);
        }

        bool bind(const std::string &host, uint16_t port, NetworkRuntime &runtime, const ListenOptions &options)
        {
            return setup(host, port, runtime, options);
        }

        void close()
        {
            if (state_) {
                state_->runtime = nullptr;
            }
            if (acceptor_) {
                acceptor_->close();
                acceptor_.reset();
            }
            runtime_ = nullptr;
        }

        void set_ssl_module(std::shared_ptr<SSLModule> ssl_module)
        {
            ssl_module_ = std::move(ssl_module);
        }

        void set_connection_handler(AsyncConnectionHandler handler)
        {
            state_->conn_handler = std::move(handler);
        }

        coroutine::Task<std::shared_ptr<Connection>> accept_async()
        {
            if (!acceptor_ || !runtime_) {
                co_return std::shared_ptr<Connection>{};
            }

            auto rv = runtime_->runtime_view();
            auto conn = co_await coroutine::async_accept(rv, acceptor_.get());
            co_return conn;
        }

        coroutine::Task<void> run_async()
        {
            if (acceptor_) {
                acceptor_->set_queue_pending_connections(false);
                acceptor_->set_connection_handler(dispatch_handler_holder_);
            }
            co_return;
        }

        bool is_listening() const noexcept
        {
            return acceptor_ != nullptr;
        }

        NetworkRuntime *runtime() const noexcept
        {
            return state_ ? state_->runtime : nullptr;
        }

        StreamAcceptor *acceptor() const noexcept
        {
            return acceptor_.get();
        }

    private:
        struct State
        {
            NetworkRuntime *runtime = nullptr;
            AsyncConnectionHandler conn_handler{};
            std::shared_ptr<ConnectionHandler> default_handler{};
        };

        static void on_connection_accepted(const std::shared_ptr<State> &state,
                                           const std::shared_ptr<Connection> &conn)
        {
            if (!state || !conn || !state->runtime) {
                return;
            }

            auto *runtime = state->runtime;
            auto *loop = runtime->event_loop();
            if (!loop) {
                return;
            }

            conn->set_event_handler(loop);
            conn->set_connection_handler(state->default_handler);

            if (auto stream = std::dynamic_pointer_cast<StreamTransport>(conn)) {
                if (auto *channel = stream->stream_channel()) {
                    loop->update_channel(channel);
                }
            }

            if (state->conn_handler) {
                auto ctx = AsyncConnectionContext(conn, static_cast<coroutine::RuntimeView>(runtime->runtime_view()));
                auto task = state->conn_handler(std::move(ctx));
                task.resume();
                task.detach();
            }
        }

        bool setup(const std::string &host, uint16_t port, NetworkRuntime &runtime, const ListenOptions &options)
        {
            if (acceptor_) {
                acceptor_->close();
                acceptor_.reset();
            }

            runtime_ = &runtime;
            state_->runtime = &runtime;
            state_->default_handler = default_handler_holder_;

            auto *loop = runtime.event_loop();
            auto *tm = runtime.timer_manager();
            if (!loop || !tm) {
                return false;
            }

            auto sock = std::make_unique<Socket>(host.c_str(), port);
            if (!sock->valid()) {
                return false;
            }

            if (!sock->apply_listen_options(options)) {
                return false;
            }
            if (!sock->bind()) {
                return false;
            }

            acceptor_.reset(create_stream_acceptor(sock.release()));
            if (!acceptor_->listen(options.backlog)) {
                acceptor_.reset();
                return false;
            }

            if (ssl_module_) {
                acceptor_->set_ssl_module(ssl_module_);
            }

            acceptor_->set_connection_handler(default_handler_holder_);
            acceptor_->set_event_handler(loop);
            if (auto *channel = acceptor_->listener_channel()) {
                loop->update_channel(channel);
            }
            return true;
        }

        class DefaultHandler final : public yuan::net::ConnectionHandler
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

        class DispatchHandler final : public yuan::net::ConnectionHandler
        {
        public:
            explicit DispatchHandler(std::weak_ptr<State> state) noexcept
                : state_(std::move(state))
            {
            }

            void on_connected(const std::shared_ptr<Connection> &conn) override
            {
                AsyncListenerHost::on_connection_accepted(state_.lock(), conn);
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

        private:
            std::weak_ptr<State> state_;
        };

        NetworkRuntime *runtime_ = nullptr;
        std::shared_ptr<State> state_ = std::make_shared<State>();
        std::unique_ptr<StreamAcceptor> acceptor_;
        std::shared_ptr<SSLModule> ssl_module_;
        DefaultHandler default_handler_;
        std::shared_ptr<ConnectionHandler> default_handler_holder_{ make_non_owning_handler(default_handler_) };
        DispatchHandler dispatch_handler_{ state_ };
        std::shared_ptr<ConnectionHandler> dispatch_handler_holder_{ make_non_owning_handler(dispatch_handler_) };
    };

} // namespace yuan::net

#endif
