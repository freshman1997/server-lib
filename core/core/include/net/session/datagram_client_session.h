#ifndef __YUAN_NET_SESSION_DATAGRAM_CLIENT_SESSION_H__
#define __YUAN_NET_SESSION_DATAGRAM_CLIENT_SESSION_H__

#include <functional>
#include <memory>
#include <string>

#include "coroutine/completion_event.h"
#include "coroutine/connection_event_awaitable.h"
#include "coroutine/runtime.h"
#include "coroutine/task.h"
#include "event/event_loop.h"
#include "net/acceptor/acceptor_factory.h"
#include "net/acceptor/datagram_acceptor.h"
#include "net/acceptor/udp/udp_instance.h"
#include "net/connection/connection.h"
#include "net/connection/datagram_transport.h"
#include "net/handler/connection_handler.h"
#include "net/runtime/network_runtime.h"
#include "net/session/connection_context.h"
#include "net/socket/inet_address.h"
#include "net/socket/socket.h"
#include "timer/timer_handle.h"
#include "timer/timer_manager.h"
#include "timer/timer_util.hpp"

namespace yuan::net
{

    class DatagramClientSession : public ConnectionHandler
    {
    public:
        struct SendResult
        {
            bool success = false;
            int bytes_sent = 0;
        };

        DatagramClientSession() = default;

        ~DatagramClientSession()
        {
            close();
        }

        DatagramClientSession(const DatagramClientSession &) = delete;
        DatagramClientSession &operator=(const DatagramClientSession &) = delete;

        bool connect(const std::string &host, uint16_t port, NetworkRuntime &runtime)
        {
            runtime_ = &runtime;
            return setup(host, port);
        }

        void close()
        {
            connection_.reset();
            if (acceptor_) {
                acceptor_->close();
                acceptor_.reset();
            }
            if (timeout_timer_) {
                timeout_timer_.cancel();
                timeout_timer_.reset();
            }
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
            return coroutine::RuntimeView(external_loop_, external_timer_manager_);
        }

        ConnectionContext context() const noexcept
        {
            return ConnectionContext(connection_);
        }

        timer::TimerHandle schedule(uint32_t delay_ms, std::function<void()> callback)
        {
            return runtime_ ? runtime_->schedule_handle(delay_ms, std::move(callback)) : timer::TimerHandle{};
        }

        void cancel_timer(const timer::TimerHandle &timer)
        {
            if (runtime_) {
                runtime_->cancel_timer(timer);
            }
        }

        void dispatch(std::function<void()> callback)
        {
            if (runtime_) {
                runtime_->dispatch(std::move(callback));
            }
        }

        bool is_connected() const noexcept
        {
            return connection_ != nullptr;
        }

        void send(const ::yuan::buffer::ByteBuffer &buffer)
        {
            if (connection_ && acceptor_) {
                auto *endpoint = static_cast<DatagramEndpoint *>(acceptor_);
                endpoint->send_datagram(connection_, buffer);
            }
        }

        void send_to(const InetAddress &addr, const ::yuan::buffer::ByteBuffer &buffer)
        {
            if (acceptor_) {
                auto *endpoint = static_cast<DatagramEndpoint *>(acceptor_);
                endpoint->send_datagram(addr, buffer);
            }
        }

        yuan::coroutine::Task< ::yuan::buffer::ByteBuffer> receive_async(uint32_t timeout_ms = 0)
        {
            auto rv = runtime_view();
            if (!connection_ || !rv.event_loop()) {
                co_return::yuan::buffer::ByteBuffer{};
            }

            if (connection_->input_readable_bytes() > 0) {
                co_return connection_->take_input_byte_buffer();
            }

            if (timeout_ms > 0 && timer_manager()) {
                completion_event_.reset(event_loop());
                auto timed_out = co_await completion_event_.wait_for(timer_manager(), timeout_ms);
                if (timed_out) {
                    co_return::yuan::buffer::ByteBuffer{};
                }
                co_return connection_->take_input_byte_buffer();
            }

            auto exit_reason = co_await yuan::coroutine::wait_readable(rv, connection_);
            if (exit_reason != EventLoopExitReason::coroutine_resume_requested) {
                co_return::yuan::buffer::ByteBuffer{};
            }

            co_return connection_->take_input_byte_buffer();
        }

        yuan::coroutine::Task<bool> send_and_receive_async(
            const ::yuan::buffer::ByteBuffer &send_buf,
            ::yuan::buffer::ByteBuffer &recv_buf,
            uint32_t timeout_ms = 0)
        {
            auto rv = runtime_view();
            if (!connection_ || !rv.event_loop()) {
                co_return false;
            }

            send(send_buf);

            if (connection_->input_readable_bytes() > 0) {
                recv_buf = connection_->take_input_byte_buffer();
                co_return true;
            }

            if (timeout_ms > 0 && timer_manager()) {
                completion_event_.reset(event_loop());
                auto timed_out = co_await completion_event_.wait_for(timer_manager(), timeout_ms);
                if (timed_out) {
                    co_return false;
                }
                recv_buf = connection_->take_input_byte_buffer();
                co_return true;
            }

            auto exit_reason = co_await yuan::coroutine::wait_readable(rv, connection_);
            if (exit_reason != EventLoopExitReason::coroutine_resume_requested) {
                co_return false;
            }

            recv_buf = connection_->take_input_byte_buffer();
            co_return true;
        }

        void on_connected(const std::shared_ptr<Connection> &conn) override
        {
            (void)conn;
        }

        void on_error(const std::shared_ptr<Connection> &conn) override
        {
            if (completion_event_.completed()) {
                return;
            }
            completion_event_.notify();
        }

        void on_read(const std::shared_ptr<Connection> &conn) override
        {
            if (!completion_event_.completed()) {
                completion_event_.notify();
            }
        }

        void on_write(const std::shared_ptr<Connection> &conn) override
        {
            (void)conn;
        }

        void on_close(const std::shared_ptr<Connection> &conn) override
        {
            connection_.reset();
            if (completion_event_.completed()) {
                return;
            }
            completion_event_.notify();
        }

        coroutine::CompletionEvent &completion_event() noexcept
        {
            return completion_event_;
        }

    private:
        bool setup(const std::string &host, uint16_t port)
        {
            auto sock = std::make_unique<Socket>(host.c_str(), port, true);
            if (!sock->valid()) {
                return false;
            }
            const InetAddress remote_address = *sock->get_address();

            auto *tm = timer_manager();
            auto *loop = event_loop();
            if (!tm || !loop) {
                return false;
            }

            acceptor_.reset(create_datagram_acceptor(sock.release(), tm));
            if (!acceptor_->listen()) {
                acceptor_.reset();
                return false;
            }

            acceptor_->set_connection_handler(self_handler_holder_);
            acceptor_->set_event_handler(loop);
            loop->update_channel(acceptor_->endpoint_channel());

            auto instance = acceptor_->get_udp_instance();
            auto res = instance->on_recv(remote_address);
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

            conn->set_connection_handler(self_handler_holder_);
            conn->set_event_handler(loop);
            datagram->attach_datagram_instance(instance);
            datagram->set_datagram_state(ConnectionState::connected);
            instance->enable_rw_events();

            connection_ = conn;
            completion_event_.reset(loop);
            return true;
        }

        NetworkRuntime *runtime_ = nullptr;
        EventLoop *external_loop_ = nullptr;
        timer::TimerManager *external_timer_manager_ = nullptr;

        std::shared_ptr<Connection> connection_;
        std::unique_ptr<DatagramAcceptor> acceptor_;
        timer::TimerHandle timeout_timer_;
        coroutine::CompletionEvent completion_event_;
        std::shared_ptr<ConnectionHandler> self_handler_holder_{ make_non_owning_handler(*this) };
    };

} // namespace yuan::net

#endif
