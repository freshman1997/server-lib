#ifndef __YUAN_COROUTINE_DATAGRAM_IO_AWAITABLE_H__
#define __YUAN_COROUTINE_DATAGRAM_IO_AWAITABLE_H__

#include <array>
#include <coroutine>
#include <memory>

#include "coroutine/awaiter_timeout_state.h"
#include "coroutine/io_result.h"
#include "coroutine/runtime_view.h"
#include "net/acceptor/datagram_endpoint.h"
#include "net/connection/connection.h"
#include "net/connection/connection_handle.h"
#include "net/handler/connection_handler.h"
#include "net/socket/inet_address.h"
#include "timer/timer_manager.h"

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
        AsyncReceiveFromAwaiter(RuntimeView runtime,
                                std::shared_ptr<net::Connection> connection,
                                uint32_t timeout_ms = 0) noexcept
            : runtime_(runtime),
              connection_handle_(net::ConnectionHandle(std::move(connection))),
              timeout_ms_(timeout_ms)
        {
        }

        bool await_ready() const noexcept
        {
            auto *connection = connection_handle_.get();
            if (!connection || !runtime_.event_loop()) {
                return true;
            }
            return connection->input_readable_bytes() > 0;
        }

        bool await_suspend(std::coroutine_handle<> handle)
        {
            auto *connection = connection_handle_.get();
            if (!connection || !runtime_.event_loop()) {
                result_.status = IoStatus::invalid_state;
                return false;
            }

            if (connection->input_readable_bytes() > 0) {
                return false;
            }

            handle_ = handle;
            net::Connection::EventWaiterRegistration registrations[] = {
                { net::ConnectionEvent::readable, [this](net::Connection &) {
                    if (!completed_) {
                        complete(IoStatus::success);
                    }
                } },
                { net::ConnectionEvent::error, [this](net::Connection &) {
                    if (!completed_) {
                        complete(IoStatus::connection_error);
                    }
                } },
                { net::ConnectionEvent::closed, [this](net::Connection &) {
                    if (!completed_) {
                        complete(IoStatus::connection_closed);
                    }
                } },
            };
            waiter_count_ = connection->add_event_waiters(registrations,
                                                          sizeof(registrations) / sizeof(registrations[0]),
                                                          waiter_ids_.data(),
                                                          waiter_ids_.size());

            if (timeout_ms_ > 0 && runtime_.timer_manager()) {
                timeout_state_ = std::make_shared<detail::AwaiterTimeoutState>();
                timeout_state_->loop = runtime_.event_loop();
                timeout_state_->handle = handle;
                detail::arm_awaiter_timeout(runtime_, timeout_ms_, timeout_state_);
            }

            return true;
        }

        ReadResult await_resume() noexcept
        {
            restore_handler_if_needed();

            if (timeout_state_ && timeout_state_->timed_out && !completed_) {
                completed_ = true;
                result_.status = IoStatus::timed_out;
            }
            detail::cancel_awaiter_timeout(timeout_state_);

            auto *connection = connection_handle_.get();
            if (result_.status == IoStatus::success && connection) {
                result_.data = connection->take_input_byte_buffer();
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
            if (timeout_state_) {
                timeout_state_->completed = true;
            }
            restore_handler_if_needed();
            if (runtime_.event_loop()) {
                runtime_.event_loop()->post_coroutine(handle_);
            }
        }
        void restore_handler_if_needed() noexcept
        {
            auto *connection = connection_handle_.get();
            cancel_waiters(connection);
            if (handler_restored_) {
                return;
            }
            handler_restored_ = true;
        }

        void cancel_waiters(net::Connection *connection) noexcept
        {
            if (!connection || waiter_count_ == 0) {
                waiter_count_ = 0;
                return;
            }
            connection->remove_event_waiters(waiter_ids_.data(), waiter_count_);
            waiter_count_ = 0;
        }
        RuntimeView runtime_{};
        net::ConnectionHandle connection_handle_{};
        uint32_t timeout_ms_ = 0;
        std::shared_ptr<detail::AwaiterTimeoutState> timeout_state_;

        std::coroutine_handle<> handle_{};
        ReadResult result_{};
        std::array<uint64_t, 3> waiter_ids_{};
        std::size_t waiter_count_ = 0;
        bool completed_ = false;
        bool handler_restored_ = false;

    public:
        ~AsyncReceiveFromAwaiter()
        {
            detail::cancel_awaiter_timeout(timeout_state_);
        }
    };

    inline AsyncReceiveFromAwaiter async_receive_from(
        RuntimeView runtime,
        const std::shared_ptr<net::Connection> &connection,
        uint32_t timeout_ms = 0) noexcept
    {
        return AsyncReceiveFromAwaiter(runtime, connection, timeout_ms);
    }

} // namespace yuan::coroutine

#endif
