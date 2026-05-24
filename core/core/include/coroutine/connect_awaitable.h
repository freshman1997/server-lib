#ifndef __YUAN_COROUTINE_CONNECT_AWAITABLE_H__
#define __YUAN_COROUTINE_CONNECT_AWAITABLE_H__

#include <array>
#include <coroutine>
#include <memory>
#include <string>
#include <system_error>

#include "coroutine/awaiter_timeout_state.h"
#include "coroutine/connection_event_awaitable.h"
#include "coroutine/runtime.h"
#include "net/connection/connection.h"
#include "net/connection/connection_factory.h"
#include "net/connection/stream_transport.h"
#include "net/handler/connection_handler.h"
#include "net/socket/inet_address.h"
#include "net/socket/socket.h"
#include "timer/timer_manager.h"

namespace yuan::coroutine
{

    enum class ConnectResult {
        success,
        invalid_address,
        socket_error,
        connect_failed,
        timed_out,
        connection_error,
    };

    struct ConnectAwaitableResult
    {
        std::shared_ptr<net::Connection> connection;
        ConnectResult result = ConnectResult::connect_failed;
    };

    class ConnectAwaitable
    {
    public:
        ConnectAwaitable(RuntimeView runtime,
                         const std::string &host,
                         uint16_t port,
                         uint32_t timeout_ms = 0) noexcept
            : runtime_(runtime),
              host_(host),
              port_(port),
              timeout_ms_(timeout_ms)
        {
        }

        bool await_ready() const noexcept
        {
            return false;
        }

        bool await_suspend(std::coroutine_handle<> handle)
        {
            handle_ = handle;

            net::InetAddress addr{ host_.c_str(), port_ };
            if (addr.get_ip().empty()) {
                result_.result = ConnectResult::invalid_address;
                return false;
            }

            int connect_errno = 0;
            auto sock = std::make_unique<net::Socket>(addr.get_ip().c_str(), port_);
            if (!sock->valid()) {
                result_.result = ConnectResult::socket_error;
                return false;
            }

            sock->set_none_block(true);
            if (!sock->connect()) {
                connect_errno = sock->last_error();
                result_.result = ConnectResult::connect_failed;
                return false;
            }

            conn_ = net::create_stream_connection(sock.release());
            if (!conn_) {
                result_.result = ConnectResult::socket_error;
                return false;
            }

            net::Connection::EventWaiterRegistration registrations[] = {
                { net::ConnectionEvent::connected, [this](const std::shared_ptr<net::Connection> &) {
                    if (!completed_) {
                        result_.result = ConnectResult::success;
                        resume();
                    }
                } },
                { net::ConnectionEvent::error, [this](const std::shared_ptr<net::Connection> &) {
                    if (!completed_) {
                        result_.result = ConnectResult::connection_error;
                        resume();
                    }
                } },
                { net::ConnectionEvent::closed, [this](const std::shared_ptr<net::Connection> &) {
                    if (!completed_) {
                        result_.result = ConnectResult::connection_error;
                        resume();
                    }
                } },
            };
            waiter_count_ = conn_->add_event_waiters(registrations,
                                                     sizeof(registrations) / sizeof(registrations[0]),
                                                     waiter_ids_.data(),
                                                     waiter_ids_.size());
            conn_->set_event_handler(runtime_.event_loop());

            if (connect_errno != 0) {
                result_.result = ConnectResult::connect_failed;
            }

            if (auto *stream = dynamic_cast<net::StreamTransport *>(&*conn_)) {
                if (auto *channel = stream->stream_channel()) {
                    runtime_.event_loop()->update_channel(channel);
                }
            }

            if (timeout_ms_ > 0 && runtime_.timer_manager()) {
                timeout_state_ = std::make_shared<detail::AwaiterTimeoutState>();
                timeout_state_->loop = runtime_.event_loop();
                timeout_state_->handle = handle;
                detail::arm_awaiter_timeout(runtime_, timeout_ms_, timeout_state_);
            }

            return true;
        }

        ConnectAwaitableResult await_resume() noexcept
        {
            const bool timed_out = timeout_state_ && timeout_state_->timed_out && !completed_;
            if (timed_out) {
                completed_ = true;
            }
            detail::cancel_awaiter_timeout(timeout_state_);

            cancel_waiters();

            if (timed_out) {
                if (conn_) {
                    conn_->abort();
                    conn_ = nullptr;
                }
                result_.result = ConnectResult::timed_out;
            }

            if (result_.result == ConnectResult::success) {
                result_.connection = conn_;
            } else if (conn_) {
                conn_->abort();
                conn_ = nullptr;
            }

            return result_;
        }

    private:
        void cancel_waiters() noexcept
        {
            if (!conn_ || waiter_count_ == 0) {
                waiter_count_ = 0;
                return;
            }
            conn_->remove_event_waiters(waiter_ids_.data(), waiter_count_);
            waiter_count_ = 0;
        }

        void resume() noexcept
        {
            if (completed_ || !handle_) {
                return;
            }
            completed_ = true;
            if (timeout_state_) {
                timeout_state_->completed = true;
            }
            if (runtime_.event_loop()) {
                runtime_.event_loop()->post_coroutine(handle_);
            }
        }

        RuntimeView runtime_{};
        std::string host_;
        uint16_t port_ = 0;
        uint32_t timeout_ms_ = 0;
        std::shared_ptr<net::Connection> conn_;
        std::shared_ptr<detail::AwaiterTimeoutState> timeout_state_;

        std::coroutine_handle<> handle_{};
        std::array<uint64_t, 3> waiter_ids_{};
        std::size_t waiter_count_ = 0;
        ConnectAwaitableResult result_{};
        bool completed_ = false;
    };

    inline ConnectAwaitable async_connect(
        RuntimeView runtime,
        const std::string & host,
        uint16_t port,
        uint32_t timeout_ms = 0) noexcept
    {
        return ConnectAwaitable(runtime, host, port, timeout_ms);
    }

} // namespace yuan::coroutine

#endif
