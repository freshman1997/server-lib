#ifndef __YUAN_COROUTINE_DATAGRAM_IO_AWAITABLE_H__
#define __YUAN_COROUTINE_DATAGRAM_IO_AWAITABLE_H__

#include <coroutine>
#include <memory>

#include "coroutine/io_result.h"
#include "coroutine/runtime_view.h"
#include "net/acceptor/datagram_endpoint.h"
#include "net/connection/connection.h"
#include "net/handler/connection_handler.h"
#include "net/socket/inet_address.h"
#include "timer/timer_manager.h"
#include "timer/timer_util.hpp"

namespace yuan::coroutine
{

    inline DatagramSendResult async_send_to(
        net::DatagramEndpoint * endpoint,
        const net::InetAddress & addr,
        const ::yuan::buffer::ByteBuffer & buffer) noexcept
    {
        if (!endpoint) {
            return { 0, IoStatus::invalid_state };
        }

        int sent = endpoint->send_datagram(addr, buffer);
        if (sent < 0) {
            return { 0, IoStatus::connection_error };
        }
        return { sent, IoStatus::success };
    }

    class AsyncReceiveFromAwaiter
    {
    public:
        AsyncReceiveFromAwaiter(RuntimeView runtime, net::Connection *connection,
                                uint32_t timeout_ms = 0) noexcept
            : runtime_(runtime),
              connection_(connection),
              timeout_ms_(timeout_ms)
        {
        }

        bool await_ready() const noexcept
        {
            if (!connection_ || !runtime_.event_loop()) {
                return true;
            }
            return connection_->input_readable_bytes() > 0;
        }

        bool await_suspend(std::coroutine_handle<> handle)
        {
            if (!connection_ || !runtime_.event_loop()) {
                result_.status = IoStatus::invalid_state;
                return false;
            }

            if (connection_->input_readable_bytes() > 0) {
                return false;
            }

            handle_ = handle;
            proxy_ = std::make_shared<ProxyHandler>(*this, connection_,
                                                    connection_->get_connection_handler(),
                                                    connection_->get_connection_handler_owner());
            connection_->set_connection_handler(proxy_);

            if (timeout_ms_ > 0 && runtime_.timer_manager()) {
                timeout_timer_ = timer::TimerUtil::build_timeout_timer(
                    runtime_.timer_manager(),
                    timeout_ms_,
                    [this](timer::Timer *timer) {
                    if (completed_) {
                        return;
                    }
                    timed_out_ = true;
                    timeout_timer_ = nullptr;
                    if (timer) {
                        timer->cancel();
                    }
                    complete(IoStatus::timed_out);
                    });
            }

            return true;
        }

        ReadResult await_resume() noexcept
        {
            restore_handler_if_needed();
            proxy_.reset();

            if (timeout_timer_) {
                timeout_timer_->cancel();
                timeout_timer_ = nullptr;
            }

            if (result_.status == IoStatus::success && connection_) {
                result_.data = connection_->take_input_byte_buffer();
            }

            return result_;
        }

    private:
        void complete(IoStatus status) noexcept
        {
            if (completed_ || !handle_) {
                return;
            }
            completed_ = true;
            result_.status = status;
            restore_handler_if_needed();
            if (runtime_.event_loop()) {
                runtime_.event_loop()->post_coroutine(handle_);
            }
        }

        class ProxyHandler final : public net::ConnectionHandler
        {
        public:
            ProxyHandler(AsyncReceiveFromAwaiter &owner, net::Connection *connection,
                         net::ConnectionHandler *next,
                         std::shared_ptr<net::ConnectionHandler> next_owner) noexcept
                : owner_(owner),
                  connection_(connection),
                  next_(next),
                  next_owner_(std::move(next_owner))
            {
            }

            void on_connected(const std::shared_ptr<net::Connection> &conn) override
            {
                if (next_) {
                    next_->on_connected(conn);
                }
            }

            void on_error(const std::shared_ptr<net::Connection> &conn) override
            {
                if (!owner_.completed_) {
                    owner_.complete(IoStatus::connection_error);
                }
                if (next_) {
                    next_->on_error(conn);
                }
            }

            void on_read(const std::shared_ptr<net::Connection> &conn) override
            {
                if (!owner_.completed_) {
                    owner_.complete(IoStatus::success);
                    return;
                }
                if (next_) {
                    next_->on_read(conn);
                }
            }

            void on_write(const std::shared_ptr<net::Connection> &conn) override
            {
                if (next_) {
                    next_->on_write(conn);
                }
            }

            void on_close(const std::shared_ptr<net::Connection> &conn) override
            {
                if (!owner_.completed_) {
                    owner_.complete(IoStatus::connection_closed);
                }
                if (next_) {
                    next_->on_close(conn);
                }
            }

            AsyncReceiveFromAwaiter &owner_;
            net::Connection *connection_;
            net::ConnectionHandler *next_;
            std::shared_ptr<net::ConnectionHandler> next_owner_;
        };

        void restore_handler_if_needed() noexcept
        {
            if (handler_restored_ || !proxy_ || !connection_) {
                return;
            }
            if (connection_->get_connection_handler() == proxy_.get()) {
                if (proxy_->next_owner_) {
                    connection_->set_connection_handler(proxy_->next_owner_);
                } else {
                    connection_->set_connection_handler(make_non_owning_handler(proxy_->next_));
                }
            }
            handler_restored_ = true;
        }

        RuntimeView runtime_{};
        net::Connection *connection_ = nullptr;
        uint32_t timeout_ms_ = 0;
        timer::Timer *timeout_timer_ = nullptr;

        std::coroutine_handle<> handle_{};
        std::shared_ptr<ProxyHandler> proxy_;
        ReadResult result_{};
        bool completed_ = false;
        bool timed_out_ = false;
        bool handler_restored_ = false;
    };

    inline AsyncReceiveFromAwaiter async_receive_from(
        RuntimeView runtime,
        net::Connection * connection,
        uint32_t timeout_ms = 0) noexcept
    {
        return AsyncReceiveFromAwaiter(runtime, connection, timeout_ms);
    }

    inline AsyncReceiveFromAwaiter async_receive_from(
        RuntimeView runtime,
        const std::shared_ptr<net::Connection> &connection,
        uint32_t timeout_ms = 0) noexcept
    {
        return AsyncReceiveFromAwaiter(runtime, connection.get(), timeout_ms);
    }

} // namespace yuan::coroutine

#endif

