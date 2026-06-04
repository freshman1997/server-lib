#ifndef __YUAN_COROUTINE_STREAM_IO_AWAITABLE_H__
#define __YUAN_COROUTINE_STREAM_IO_AWAITABLE_H__

#include <coroutine>
#include <array>
#include <memory>

#include "coroutine/awaiter_timeout_state.h"
#include "coroutine/io_result.h"
#include "coroutine/runtime_view.h"
#include "net/connection/connection.h"
#include "net/connection/connection_handle.h"
#include "net/handler/connection_handler.h"
#include "net/security/ssl_handler.h"
#include "timer/timer_manager.h"

namespace yuan::coroutine
{

    class AsyncReadAwaiter
    {
    public:
        AsyncReadAwaiter(RuntimeView runtime,
                         std::shared_ptr<net::Connection> connection,
                         uint32_t timeout_ms = 0,
                         bool complete_with_buffered_data_on_terminal_event = true) noexcept
            : runtime_(runtime),
              connection_handle_(net::ConnectionHandle(std::move(connection))),
              timeout_ms_(timeout_ms),
              complete_with_buffered_data_on_terminal_event_(complete_with_buffered_data_on_terminal_event)
        {
        }

        bool await_ready() const noexcept
        {
            auto *connection = connection_handle_.get();
            if (!connection || !runtime_.event_loop()) {
                return true;
            }
            if (connection->get_connection_state() == net::ConnectionState::closed) {
                return true;
            }
            return connection->input_readable_bytes() > 0 || connection->input_shutdown();
        }

        bool await_suspend(std::coroutine_handle<> handle)
        {
            auto *connection = connection_handle_.get();
            if (!connection || !runtime_.event_loop()) {
                result_.status = IoStatus::invalid_state;
                return false;
            }

            if (connection->get_connection_state() == net::ConnectionState::closed) {
                result_.status = IoStatus::connection_closed;
                return false;
            }

            if (connection->input_readable_bytes() > 0) {
                return false;
            }

            if (connection->input_shutdown()) {
                result_.status = IoStatus::connection_closed;
                return false;
            }

            if (connection->has_event_waiter(net::ConnectionEvent::readable)) {
                result_.status = IoStatus::invalid_state;
                return false;
            }

            if (connection->pending_read_coroutine()) {
                result_.status = IoStatus::invalid_state;
                return false;
            }

            handle_ = handle;
            connection->set_pending_read_coroutine(handle, [this](std::coroutine_handle<>) noexcept {
                complete(IoStatus::success);
            });

            auto terminal = [this](net::Connection &conn) {
                if (!completed_) {
                    auto h = conn.take_pending_read_coroutine();
                    if (!h) {
                        return;
                    }
                    if (complete_with_buffered_data_on_terminal_event_ && conn.input_readable_bytes() > 0) {
                        complete(IoStatus::success);
                    } else {
                        complete(IoStatus::connection_closed);
                    }
                    return;
                }

            };
            net::Connection::EventWaiterRegistration registrations[] = {
                { net::ConnectionEvent::error, [this](net::Connection &conn) {
                    if (!completed_) {
                        auto h = conn.take_pending_read_coroutine();
                        if (!h) {
                            return;
                        }
                        complete(IoStatus::connection_error);
                    }
                } },
                { net::ConnectionEvent::closed, terminal },
                { net::ConnectionEvent::input_shutdown, std::move(terminal) },
            };
            waiter_count_ = connection->add_event_waiters(registrations,
                                                           sizeof(registrations) / sizeof(registrations[0]),
                                                           waiter_ids_.data(),
                                                           waiter_ids_.size());

            if (connection->input_readable_bytes() > 0) {
                result_.status = IoStatus::success;
                connection->clear_pending_read_coroutine();
                restore_handler_if_needed();
                return false;
            }

            if (connection->input_shutdown()) {
                result_.status = IoStatus::connection_closed;
                connection->clear_pending_read_coroutine();
                restore_handler_if_needed();
                return false;
            }

            if (connection->get_connection_state() != net::ConnectionState::connected) {
                result_.status = IoStatus::connection_closed;
                connection->clear_pending_read_coroutine();
                restore_handler_if_needed();
                return false;
            }

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
            if (!completed_) {
                completed_ = true;
                result_.status = IoStatus::success;
            }
            restore_handler_if_needed();

            auto *connection = connection_handle_.get();
            if (!connection || !runtime_.event_loop()) {
                return ReadResult::with_status(IoStatus::invalid_state);
            }

            if (timeout_state_ && timeout_state_->timed_out && !completed_) {
                completed_ = true;
                result_.status = IoStatus::timed_out;
            }
            detail::cancel_awaiter_timeout(timeout_state_);

            if (result_.status == IoStatus::success) {
                result_.data = connection->take_and_clear_input_byte_buffer();
                const bool empty_read = result_.data.readable_bytes() == 0;
                if (empty_read && connection->input_shutdown()) {
                    result_.status = IoStatus::connection_closed;
                } else if (empty_read && connection->get_connection_state() == net::ConnectionState::closed) {
                    result_.status = IoStatus::connection_closed;
                }
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
        std::array<uint64_t, 4> waiter_ids_{};
        std::size_t waiter_count_ = 0;
        bool completed_ = false;
        bool handler_restored_ = false;
        bool complete_with_buffered_data_on_terminal_event_ = true;

    public:
        ~AsyncReadAwaiter()
        {
            detail::cancel_awaiter_timeout(timeout_state_);
        }
    };

    inline AsyncReadAwaiter async_read(
        RuntimeView runtime,
        const std::shared_ptr<net::Connection> &connection,
        uint32_t timeout_ms = 0) noexcept
    {
        return AsyncReadAwaiter(runtime, connection, timeout_ms);
    }

    inline AsyncReadAwaiter async_read(
        RuntimeView runtime,
        const std::shared_ptr<net::Connection> &connection,
        uint32_t timeout_ms,
        bool complete_with_buffered_data_on_terminal_event) noexcept
    {
        return AsyncReadAwaiter(runtime, connection, timeout_ms, complete_with_buffered_data_on_terminal_event);
    }

    class AsyncWriteAwaiter
    {
    public:
        AsyncWriteAwaiter(RuntimeView runtime,
                          std::shared_ptr<net::Connection> connection,
                          ::yuan::buffer::ByteBuffer buffer,
                          uint32_t timeout_ms = 0) noexcept
            : runtime_(runtime),
              connection_handle_(net::ConnectionHandle(std::move(connection))),
              buffer_(std::move(buffer)),
              timeout_ms_(timeout_ms)
        {
        }

        bool await_ready() const noexcept
        {
            auto *connection = connection_handle_.get();
            return !connection || !runtime_.event_loop();
        }

        bool await_suspend(std::coroutine_handle<> handle)
        {
            auto *connection = connection_handle_.get();
            if (!connection || !runtime_.event_loop()) {
                result_.status = IoStatus::invalid_state;
                return false;
            }

            if (connection->get_connection_state() == net::ConnectionState::closed) {
                result_.status = IoStatus::connection_closed;
                return false;
            }

            handle_ = handle;
            net::Connection::EventWaiterRegistration registrations[] = {
                { net::ConnectionEvent::writable, [this](net::Connection &conn) {
                    if (!completed_ && conn.output_readable_bytes() == 0) {
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
            connection->write_and_flush(buffer_);

            if (completed_) {
                return true;
            }
            if (connection->output_readable_bytes() == 0) {
                result_.status = IoStatus::success;
                restore_handler_if_needed();
                return false;
            }

            if (timeout_ms_ > 0 && runtime_.timer_manager()) {
                timeout_state_ = std::make_shared<detail::AwaiterTimeoutState>();
                timeout_state_->loop = runtime_.event_loop();
                timeout_state_->handle = handle;
                detail::arm_awaiter_timeout(runtime_, timeout_ms_, timeout_state_);
            }

            return true;
        }

        WriteResult await_resume() noexcept
        {
            restore_handler_if_needed();

            if (timeout_state_ && timeout_state_->timed_out && !completed_) {
                completed_ = true;
                result_.status = IoStatus::timed_out;
            }
            detail::cancel_awaiter_timeout(timeout_state_);

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
        ::yuan::buffer::ByteBuffer buffer_;
        uint32_t timeout_ms_ = 0;
        std::shared_ptr<detail::AwaiterTimeoutState> timeout_state_;

        std::coroutine_handle<> handle_{};
        WriteResult result_{};
        std::array<uint64_t, 4> waiter_ids_{};
        std::size_t waiter_count_ = 0;
        bool completed_ = false;
        bool handler_restored_ = false;

    public:
        ~AsyncWriteAwaiter()
        {
            detail::cancel_awaiter_timeout(timeout_state_);
        }
    };

    inline AsyncWriteAwaiter async_write(
        RuntimeView runtime,
        const std::shared_ptr<net::Connection> &connection,
        ::yuan::buffer::ByteBuffer buffer,
        uint32_t timeout_ms = 0) noexcept
    {
        return AsyncWriteAwaiter(runtime, connection, std::move(buffer), timeout_ms);
    }

    class AsyncFlushAwaiter
    {
    public:
        AsyncFlushAwaiter(RuntimeView runtime,
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
            return !connection || !runtime_.event_loop();
        }

        bool await_suspend(std::coroutine_handle<> handle)
        {
            auto *connection = connection_handle_.get();
            if (!connection || !runtime_.event_loop()) {
                result_.status = IoStatus::invalid_state;
                return false;
            }

            if (connection->get_connection_state() == net::ConnectionState::closed) {
                result_.status = IoStatus::connection_closed;
                return false;
            }

            handle_ = handle;
            net::Connection::EventWaiterRegistration registrations[] = {
                { net::ConnectionEvent::writable, [this](net::Connection &conn) {
                    if (!completed_ && conn.output_readable_bytes() == 0) {
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
            connection->flush();

            if (completed_) {
                return true;
            }
            if (connection->output_readable_bytes() == 0) {
                result_.status = IoStatus::success;
                restore_handler_if_needed();
                return false;
            }

            if (timeout_ms_ > 0 && runtime_.timer_manager()) {
                timeout_state_ = std::make_shared<detail::AwaiterTimeoutState>();
                timeout_state_->loop = runtime_.event_loop();
                timeout_state_->handle = handle;
                detail::arm_awaiter_timeout(runtime_, timeout_ms_, timeout_state_);
            }

            return true;
        }

        WriteResult await_resume() noexcept
        {
            restore_handler_if_needed();

            if (timeout_state_ && timeout_state_->timed_out && !completed_) {
                completed_ = true;
                result_.status = IoStatus::timed_out;
            }
            detail::cancel_awaiter_timeout(timeout_state_);

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
        WriteResult result_{};
        std::array<uint64_t, 3> waiter_ids_{};
        std::size_t waiter_count_ = 0;
        bool completed_ = false;
        bool handler_restored_ = false;

    public:
        ~AsyncFlushAwaiter()
        {
            detail::cancel_awaiter_timeout(timeout_state_);
        }
    };

    inline AsyncFlushAwaiter async_flush(
        RuntimeView runtime,
        const std::shared_ptr<net::Connection> &connection,
        uint32_t timeout_ms = 0) noexcept
    {
        return AsyncFlushAwaiter(runtime, connection, timeout_ms);
    }

    class AsyncCloseAwaiter
    {
    public:
        AsyncCloseAwaiter(RuntimeView runtime, std::shared_ptr<net::Connection> connection) noexcept
            : runtime_(runtime),
              connection_handle_(net::ConnectionHandle(std::move(connection)))
        {
        }

        bool await_ready() const noexcept
        {
            auto *connection = connection_handle_.get();
            if (!connection || !runtime_.event_loop()) {
                return true;
            }
            return connection->get_connection_state() == net::ConnectionState::closed;
        }

        bool await_suspend(std::coroutine_handle<> handle)
        {
            auto *connection = connection_handle_.get();
            if (!connection || !runtime_.event_loop()) {
                result_ = IoStatus::invalid_state;
                return false;
            }

            handle_ = handle;
            net::Connection::EventWaiterRegistration registrations[] = {
                { net::ConnectionEvent::closed, [this](net::Connection &) {
                    if (!completed_) {
                        complete(IoStatus::success);
                    }
                } },
                { net::ConnectionEvent::error, [this](net::Connection &) {
                    if (!completed_) {
                        complete(IoStatus::connection_error);
                    }
                } },
                { net::ConnectionEvent::input_shutdown, [this](net::Connection &) {
                    if (!completed_) {
                        complete(IoStatus::connection_closed);
                    }
                } },
            };
            waiter_count_ = connection->add_event_waiters(registrations,
                                                          sizeof(registrations) / sizeof(registrations[0]),
                                                          waiter_ids_.data(),
                                                          waiter_ids_.size());
            connection->close();

            if (completed_) {
                return true;
            }
            if (connection->get_connection_state() == net::ConnectionState::closed) {
                result_ = IoStatus::success;
                restore_handler_if_needed();
                return false;
            }
            return true;
        }

        IoStatus await_resume() noexcept
        {
            restore_handler_if_needed();
            return result_;
        }

    private:
        void complete(IoStatus status) noexcept
        {
            if (completed_ || !handle_) {
                return;
            }
            completed_ = true;
            result_ = status;
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

        std::coroutine_handle<> handle_{};
        IoStatus result_ = IoStatus::success;
        std::array<uint64_t, 3> waiter_ids_{};
        std::size_t waiter_count_ = 0;
        bool completed_ = false;
        bool handler_restored_ = false;
    };

    inline AsyncCloseAwaiter async_close(
        RuntimeView runtime,
        const std::shared_ptr<net::Connection> &connection) noexcept
    {
        return AsyncCloseAwaiter(runtime, connection);
    }

    enum class SslHandshakeResult {
        success,
        failed,
        invalid_state,
        timed_out,
    };

    class AsyncSslHandshakeAwaiter
    {
    public:
        AsyncSslHandshakeAwaiter(RuntimeView runtime,
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
                result_ = SslHandshakeResult::invalid_state;
                return true;
            }
            auto ssl = connection->get_ssl_handler();
            if (!ssl) {
                result_ = SslHandshakeResult::invalid_state;
                return true;
            }
            return false;
        }

        bool await_suspend(std::coroutine_handle<> handle)
        {
            auto *connection = connection_handle_.get();
            if (!connection || !runtime_.event_loop()) {
                result_ = SslHandshakeResult::invalid_state;
                return false;
            }

            auto ssl = connection->get_ssl_handler();
            if (!ssl) {
                result_ = SslHandshakeResult::invalid_state;
                return false;
            }

            handle_ = handle;

            if (!connection->is_ssl_handshaking()) {
                int ret = ssl->ssl_init_action();
                if (ret > 0) {
                    result_ = SslHandshakeResult::success;
                    return false;
                }
                if (!ssl->ssl_want_read() && !ssl->ssl_want_write()) {
                    result_ = SslHandshakeResult::failed;
                    return false;
                }
                connection->set_ssl_handshaking(true);
            }

            connection->set_ssl_handshake_callback([this](bool success) {
                if (completed_) {
                    return;
                }
                completed_ = true;
                result_ = success ? SslHandshakeResult::success : SslHandshakeResult::failed;
                if (timeout_state_) {
                    timeout_state_->completed = true;
                }
                if (runtime_.event_loop()) {
                    runtime_.event_loop()->post_coroutine(handle_);
                }
            });

            if (timeout_ms_ > 0 && runtime_.timer_manager()) {
                timeout_state_ = std::make_shared<detail::AwaiterTimeoutState>();
                timeout_state_->loop = runtime_.event_loop();
                timeout_state_->handle = handle;
                detail::arm_awaiter_timeout(runtime_, timeout_ms_, timeout_state_);
            }

            return true;
        }

        SslHandshakeResult await_resume() noexcept
        {
            auto *connection = connection_handle_.get();
            if (connection) {
                connection->set_ssl_handshake_callback(nullptr);
            }

            if (timeout_state_ && timeout_state_->timed_out && !completed_) {
                completed_ = true;
                result_ = SslHandshakeResult::timed_out;
            }
            detail::cancel_awaiter_timeout(timeout_state_);

            if (connection) {
                connection->set_ssl_handshaking(false);
            }

            return result_;
        }

    private:
        void complete(SslHandshakeResult result) noexcept
        {
            if (completed_ || !handle_) {
                return;
            }
            completed_ = true;
            result_ = result;
            if (timeout_state_) {
                timeout_state_->completed = true;
            }
            auto *connection = connection_handle_.get();
            if (connection) {
                connection->set_ssl_handshake_callback(nullptr);
                connection->set_ssl_handshaking(false);
            }
            if (runtime_.event_loop()) {
                runtime_.event_loop()->post_coroutine(handle_);
            }
        }

        RuntimeView runtime_{};
        net::ConnectionHandle connection_handle_{};
        uint32_t timeout_ms_ = 0;
        std::shared_ptr<detail::AwaiterTimeoutState> timeout_state_;

        std::coroutine_handle<> handle_{};
        mutable SslHandshakeResult result_ = SslHandshakeResult::failed;
        bool completed_ = false;
    };

    inline AsyncSslHandshakeAwaiter async_ssl_handshake(
        RuntimeView runtime,
        const std::shared_ptr<net::Connection> &connection,
        uint32_t timeout_ms = 0) noexcept
    {
        return AsyncSslHandshakeAwaiter(runtime, connection, timeout_ms);
    }

} // namespace yuan::coroutine

#endif
