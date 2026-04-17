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
            connection_ = nullptr;
            if (acceptor_) {
                delete acceptor_;
                acceptor_ = nullptr;
            }
        }

        coroutine::Task<coroutine::ReadResult> receive_async(uint32_t timeout_ms = 0)
        {
            auto rv = runtime_view();
            if (!connection_ || !rv.event_loop()) {
                co_return coroutine::ReadResult::with_status(coroutine::IoStatus::invalid_state);
            }

            co_return co_await coroutine::async_receive_from(rv, connection_, timeout_ms);
        }

        coroutine::DatagramSendResult send(const ::yuan::buffer::ByteBuffer &buffer)
        {
            if (!connection_ || !acceptor_) {
                return { 0, coroutine::IoStatus::invalid_state };
            }

            auto *endpoint = static_cast<DatagramEndpoint *>(acceptor_);
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

            auto *endpoint = static_cast<DatagramEndpoint *>(acceptor_);
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

        timer::Timer *schedule(uint32_t delay_ms, std::function<void()> callback)
        {
            return runtime_view().schedule(delay_ms, std::move(callback));
        }

        timer::Timer *schedule_periodic(uint32_t delay_ms, uint32_t interval_ms,
                                        std::function<void()> callback, int repeat = 0)
        {
            return runtime_view().schedule_periodic(delay_ms, interval_ms, std::move(callback), repeat);
        }

        void cancel_timer(timer::Timer *timer)
        {
            coroutine::RuntimeView::cancel_timer(timer);
        }

    private:
        bool setup(const std::string &host, uint16_t port)
        {
            Socket *sock = new Socket(host.c_str(), port, true);
            if (!sock->valid()) {
                delete sock;
                return false;
            }

            auto rv = runtime_view();
            auto *loop = rv.event_loop();
            if (!loop) {
                delete sock;
                return false;
            }

            acceptor_ = create_datagram_acceptor(sock, rv);
            if (!acceptor_->listen()) {
                delete acceptor_;
                acceptor_ = nullptr;
                return false;
            }

            acceptor_->set_connection_handler(&default_handler_);
            acceptor_->set_event_handler(loop);
            loop->update_channel(acceptor_->endpoint_channel());

            auto instance = acceptor_->get_udp_instance();
            const auto &res = instance->on_recv(*sock->get_address());
            if (!res.first || !res.second) {
                delete acceptor_;
                acceptor_ = nullptr;
                return false;
            }

            Connection *conn = res.second;
            auto *datagram = dynamic_cast<DatagramTransport *>(conn);
            if (!datagram) {
                delete acceptor_;
                acceptor_ = nullptr;
                return false;
            }

            conn->set_connection_handler(&default_handler_);
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
            void on_connected(Connection *) override
            {
            }
            void on_error(Connection *) override
            {
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
        coroutine::RuntimeView external_runtime_{};
        Connection *connection_ = nullptr;
        DatagramAcceptor *acceptor_ = nullptr;
        DefaultHandler default_handler_;
    };

} // namespace yuan::net

#endif
