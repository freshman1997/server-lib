#ifndef __YUAN_NET_ASYNC_ASYNC_DATAGRAM_CLIENT_H__
#define __YUAN_NET_ASYNC_ASYNC_DATAGRAM_CLIENT_H__

#include <memory>
#include <string>
#include <functional>

#include "coroutine/datagram_io_awaitable.h"
#include "coroutine/io_result.h"
#include "coroutine/runtime.h"
#include "coroutine/task.h"
#include "event/event_loop.h"
#include "net/acceptor/acceptor_factory.h"
#include "net/acceptor/datagram_acceptor.h"
#include "net/acceptor/datagram_endpoint.h"
#include "net/acceptor/udp/udp_instance.h"
#include "net/async/async_connection_context.h"
#include "net/connection/connection.h"
#include "net/connection/datagram_transport.h"
#include "net/runtime/network_runtime.h"
#include "net/socket/inet_address.h"
#include "net/socket/socket.h"
#include "timer/timer.h"

namespace yuan::net
{

    class AsyncDatagramClient
    {
    public:
        AsyncDatagramClient() = default;

        ~AsyncDatagramClient()
        {
            close();
        }

        AsyncDatagramClient(const AsyncDatagramClient &) = delete;
        AsyncDatagramClient &operator=(const AsyncDatagramClient &) = delete;

        bool connect(const std::string &host, uint16_t port, NetworkRuntime &runtime)
        {
            runtime_ = &runtime;
            return setup(host, port);
        }

        bool connect(const std::string &host, uint16_t port, coroutine::RuntimeView runtime)
        {
            external_runtime_ = runtime;
            return setup(host, port);
        }

        void close()
        {
            connection_.reset();
            if (acceptor_) {
                acceptor_->close();
                acceptor_.reset();
            }
        }

        coroutine::Task<coroutine::ReadResult> receive_async(uint32_t timeout_ms = 0)
        {
            auto rv = runtime_view();
            if (!connection_ || !rv.event_loop()) {
                co_return coroutine::ReadResult::with_status(coroutine::IoStatus::invalid_state);
            }

            co_return co_await coroutine::async_receive_from(rv, connection_->shared_from_this(), timeout_ms);
        }

        coroutine::DatagramSendResult send(const ::yuan::buffer::ByteBuffer &buffer)
        {
            if (!connection_ || !acceptor_) {
                return { 0, coroutine::IoStatus::invalid_state };
            }

            auto *endpoint = static_cast<DatagramEndpoint *>(acceptor_.get());
            int sent = endpoint->send_datagram(connection_, buffer);
            if (sent < 0) {
                return { 0, coroutine::IoStatus::connection_error };
            }
            return { sent, coroutine::IoStatus::success };
        }

        coroutine::DatagramSendResult send_to(const InetAddress &addr, const ::yuan::buffer::ByteBuffer &buffer)
        {
            if (!acceptor_) {
                return { 0, coroutine::IoStatus::invalid_state };
            }

            auto *endpoint = static_cast<DatagramEndpoint *>(acceptor_.get());
            int sent = endpoint->send_datagram(addr, buffer);
            if (sent < 0) {
                return { 0, coroutine::IoStatus::connection_error };
            }
            return { sent, coroutine::IoStatus::success };
        }

        coroutine::Task<coroutine::ReadResult> send_and_receive_async(
            const ::yuan::buffer::ByteBuffer &send_buf,
            uint32_t timeout_ms = 0)
        {
            auto send_result = send(send_buf);
            if (send_result.status != coroutine::IoStatus::success) {
                co_return coroutine::ReadResult::with_status(send_result.status);
            }
            co_return co_await receive_async(timeout_ms);
        }

        bool is_connected() const noexcept
        {
            return connection_ != nullptr;
        }

        Connection *native_handle() const noexcept
        {
            return connection_ ? const_cast<Connection *>(&*connection_) : nullptr;
        }

        std::shared_ptr<Connection> connection() const noexcept
        {
            return connection_;
        }

        NetworkRuntime *runtime() const noexcept
        {
            return runtime_;
        }

        coroutine::RuntimeView runtime_view() const noexcept
        {
            if (runtime_) {
                return runtime_->runtime_view();
            }
            return external_runtime_;
        }

        timer::TimerHandle schedule(uint32_t delay_ms, std::function<void()> callback)
        {
            return runtime_view().schedule_handle(delay_ms, std::move(callback));
        }

        timer::TimerHandle schedule_periodic(uint32_t delay_ms, uint32_t interval_ms,
                                             std::function<void()> callback, int repeat = 0)
        {
            return runtime_view().schedule_periodic_handle(delay_ms, interval_ms, std::move(callback), repeat);
        }

        void cancel_timer(const timer::TimerHandle &timer)
        {
            coroutine::RuntimeView::cancel_timer(timer);
        }

    private:
        bool setup(const std::string &host, uint16_t port)
        {
            auto sock = std::make_unique<Socket>(host.c_str(), port, true);
            if (!sock->valid()) {
                return false;
            }
            const InetAddress remote_address = *sock->get_address();

            auto rv = runtime_view();
            auto *loop = rv.event_loop();
            if (!loop) {
                return false;
            }

            acceptor_.reset(create_datagram_acceptor(sock.release(), rv));
            if (!acceptor_->listen()) {
                acceptor_.reset();
                return false;
            }

            acceptor_->set_connection_handler(default_handler_holder_);
            acceptor_->set_event_handler(loop);
            loop->update_channel(acceptor_->endpoint_channel());

            auto instance = acceptor_->get_udp_instance();
            const auto &res = instance->on_recv(remote_address);
            if (!res.first || !res.second) {
                acceptor_->close();
                acceptor_.reset();
                return false;
            }

            auto conn = res.second;
            auto datagram = std::dynamic_pointer_cast<DatagramTransport>(conn);
            if (!datagram) {
                acceptor_->close();
                acceptor_.reset();
                return false;
            }

            conn->set_connection_handler(default_handler_holder_);
            conn->set_event_handler(loop);
            datagram->attach_datagram_instance(instance);
            datagram->set_datagram_state(ConnectionState::connected);
            instance->enable_rw_events();

            connection_ = conn;
            return true;
        }

        class DefaultHandler final : public ConnectionHandler
        {
        public:
            void on_connected(const std::shared_ptr<Connection> &) override
            {
            }
            void on_error(const std::shared_ptr<Connection> &) override
            {
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
        };

        NetworkRuntime *runtime_ = nullptr;
        coroutine::RuntimeView external_runtime_{};
        std::shared_ptr<Connection> connection_;
        std::unique_ptr<DatagramAcceptor> acceptor_;
        DefaultHandler default_handler_;
        std::shared_ptr<ConnectionHandler> default_handler_holder_{ make_non_owning_handler(default_handler_) };
    };

} // namespace yuan::net

#endif
