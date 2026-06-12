#ifndef __YUAN_COROUTINE_STREAM_IO_AWAITABLE_H__
#define __YUAN_COROUTINE_STREAM_IO_AWAITABLE_H__

#include <coroutine>
#include <array>
#include <atomic>
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
    namespace detail
    {
        inline void resume_on_loop(net::EventLoop *loop, std::coroutine_handle<> handle) noexcept
        {
            if (!loop || !handle) {
                return;
            }
            if (loop->is_in_loop_thread()) {
                handle.resume();
            } else {
                loop->post_coroutine(handle);
            }
        }
    }

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

            state_ = std::make_shared<SharedState>();
            state_->runtime = runtime_;
            state_->connection_handle = connection_handle_;
            state_->handle = handle;
            state_->complete_with_buffered_data_on_terminal_event = complete_with_buffered_data_on_terminal_event_;
            auto weak_state = std::weak_ptr<SharedState>(state_);
            connection->set_pending_read_coroutine(
                handle,
                [weak_state](std::coroutine_handle<>) noexcept {
                    complete_shared(weak_state, IoStatus::success);
                },
                [weak_state](net::ConnectionEvent event) noexcept {
                    auto state = weak_state.lock();
                    if (!state || state->completed.load(std::memory_order_acquire)) {
                        return;
                    }
                    auto *connection = state->connection_handle.get();
                    if (event == net::ConnectionEvent::input_shutdown &&
                        state->complete_with_buffered_data_on_terminal_event &&
                        connection && connection->input_readable_bytes() > 0) {
                        complete_shared(state, IoStatus::success);
                    } else if (event == net::ConnectionEvent::error) {
                        complete_shared(state, IoStatus::connection_error);
                    } else {
                        complete_shared(state, IoStatus::connection_closed);
                    }
                });

            if (!connection->owner_event_handler()) {
                auto terminal = [weak_state](net::Connection &conn) {
                    auto state = weak_state.lock();
                    if (state && !state->completed.load(std::memory_order_acquire)) {
                        auto h = conn.take_pending_read_coroutine();
                        if (!h) {
                            return;
                        }
                        if (state->complete_with_buffered_data_on_terminal_event && conn.input_readable_bytes() > 0) {
                            complete_shared(state, IoStatus::success);
                        } else {
                            complete_shared(state, IoStatus::connection_closed);
                        }
                    }
                };
                net::Connection::EventWaiterRegistration registrations[] = {
                    { net::ConnectionEvent::readable, [weak_state](net::Connection &conn) {
                        auto state = weak_state.lock();
                        if (state && !state->completed.load(std::memory_order_acquire)) {
                            (void)conn.take_pending_read_coroutine();
                            complete_shared(state, IoStatus::success);
                        }
                    } },
                    { net::ConnectionEvent::error, [weak_state](net::Connection &conn) {
                        auto state = weak_state.lock();
                        if (state && !state->completed.load(std::memory_order_acquire)) {
                            auto h = conn.take_pending_read_coroutine();
                            if (!h) {
                                return;
                            }
                            complete_shared(state, IoStatus::connection_error);
                        }
                    } },
                    { net::ConnectionEvent::closed, terminal },
                    { net::ConnectionEvent::input_shutdown, std::move(terminal) },
                };
                state_->waiter_count = connection->add_event_waiters(registrations,
                                                                     sizeof(registrations) / sizeof(registrations[0]),
                                                                     state_->waiter_ids.data(),
                                                                     state_->waiter_ids.size());
            }

            if (connection->input_readable_bytes() > 0) {
                result_.status = IoStatus::success;
                state_->result.status = IoStatus::success;
                connection->clear_pending_read_coroutine();
                restore_shared(state_);
                return false;
            }

            if (connection->input_shutdown()) {
                result_.status = IoStatus::connection_closed;
                state_->result.status = IoStatus::connection_closed;
                connection->clear_pending_read_coroutine();
                restore_shared(state_);
                return false;
            }

            if (connection->get_connection_state() != net::ConnectionState::connected) {
                result_.status = IoStatus::connection_closed;
                state_->result.status = IoStatus::connection_closed;
                connection->clear_pending_read_coroutine();
                restore_shared(state_);
                return false;
            }

            if (timeout_ms_ > 0 && runtime_.timer_manager()) {
                timeout_state_ = std::make_shared<detail::AwaiterTimeoutState>();
                timeout_state_->loop = runtime_.event_loop();
                timeout_state_->handle = handle;
                state_->timeout_state = timeout_state_;
                detail::arm_awaiter_timeout(runtime_, timeout_ms_, timeout_state_);
            }

            return true;
        }

        ReadResult await_resume() noexcept
        {
            if (state_) {
                result_ = state_->result;
            }
            auto *connection = connection_handle_.get();
            if (!connection || !runtime_.event_loop()) {
                return ReadResult::with_status(IoStatus::invalid_state);
            }

            if (timeout_state_ && timeout_state_->timed_out &&
                state_ && !state_->completed.exchange(true, std::memory_order_acq_rel)) {
                result_.status = IoStatus::timed_out;
                state_->result.status = IoStatus::timed_out;
            }
            if (state_ && !state_->completed.exchange(true, std::memory_order_acq_rel) &&
                state_->result.status == IoStatus::success) {
                result_.status = state_->result.status;
            }
            restore_shared(state_);
            detail::cancel_awaiter_timeout(timeout_state_);

            connection->clear_pending_read_coroutine();
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
        struct SharedState
        {
            RuntimeView runtime{};
            net::ConnectionHandle connection_handle{};
            std::shared_ptr<detail::AwaiterTimeoutState> timeout_state;
            std::coroutine_handle<> handle{};
            ReadResult result{};
            std::array<uint64_t, 4> waiter_ids{};
            std::size_t waiter_count = 0;
            std::atomic_bool completed{false};
            std::atomic_bool handler_restored{false};
            bool complete_with_buffered_data_on_terminal_event = true;
        };

        static void cancel_shared_waiters(const std::shared_ptr<SharedState> &state) noexcept
        {
            if (!state || state->waiter_count == 0) {
                return;
            }
            if (auto *connection = state->connection_handle.get()) {
                connection->remove_event_waiters(state->waiter_ids.data(), state->waiter_count);
            }
            state->waiter_count = 0;
        }

        static void restore_shared(const std::shared_ptr<SharedState> &state) noexcept
        {
            if (!state || state->handler_restored.exchange(true, std::memory_order_acq_rel)) {
                return;
            }
            cancel_shared_waiters(state);
        }

        static void complete_shared(const std::weak_ptr<SharedState> &weak_state, IoStatus status) noexcept
        {
            complete_shared(weak_state.lock(), status);
        }

        static void complete_shared(const std::shared_ptr<SharedState> &state, IoStatus status) noexcept
        {
            if (!state || !state->handle || state->completed.exchange(true, std::memory_order_acq_rel)) {
                return;
            }
            state->result.status = status;
            if (state->timeout_state) {
                state->timeout_state->completed = true;
            }
            restore_shared(state);
            if (auto *loop = state->runtime.event_loop()) {
                loop->post_coroutine(state->handle);
            }
        }

        RuntimeView runtime_{};
        net::ConnectionHandle connection_handle_{};
        uint32_t timeout_ms_ = 0;
        std::shared_ptr<detail::AwaiterTimeoutState> timeout_state_;
        std::shared_ptr<SharedState> state_;

        ReadResult result_{};
        bool complete_with_buffered_data_on_terminal_event_ = true;

    public:
        ~AsyncReadAwaiter()
        {
            restore_shared(state_);
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
                    if (!completed_.load(std::memory_order_acquire) && conn.output_readable_bytes() == 0) {
                        complete(IoStatus::success);
                    }
                } },
                { net::ConnectionEvent::error, [this](net::Connection &) {
                    if (!completed_.load(std::memory_order_acquire)) {
                        complete(IoStatus::connection_error);
                    }
                } },
                { net::ConnectionEvent::closed, [this](net::Connection &) {
                    if (!completed_.load(std::memory_order_acquire)) {
                        complete(IoStatus::connection_closed);
                    }
                } },
            };
            waiter_count_ = connection->add_event_waiters(registrations,
                                                          sizeof(registrations) / sizeof(registrations[0]),
                                                          waiter_ids_.data(),
                                                          waiter_ids_.size());
            connection->write_and_flush(buffer_);

            if (completed_.load(std::memory_order_acquire)) {
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
            if (timeout_state_ && timeout_state_->timed_out &&
                !completed_.exchange(true, std::memory_order_acq_rel)) {
                result_.status = IoStatus::timed_out;
            }
            restore_handler_if_needed();
            detail::cancel_awaiter_timeout(timeout_state_);

            return result_;
        }

    private:
        void complete(IoStatus status) noexcept
        {
            if (!handle_ || completed_.exchange(true, std::memory_order_acq_rel)) {
                return;
            }
            result_.status = status;
            if (timeout_state_) {
                timeout_state_->completed = true;
            }
            restore_handler_if_needed();
            detail::resume_on_loop(runtime_.event_loop(), handle_);
        }
        void restore_handler_if_needed() noexcept
        {
            auto *connection = connection_handle_.get();
            cancel_waiters(connection);
            if (handler_restored_.exchange(true, std::memory_order_acq_rel)) {
                return;
            }
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
        std::atomic_bool completed_{false};
        std::atomic_bool handler_restored_{false};

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
            detail::resume_on_loop(runtime_.event_loop(), handle_);
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
            detail::resume_on_loop(runtime_.event_loop(), handle_);
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
                detail::resume_on_loop(runtime_.event_loop(), handle_);
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
            detail::resume_on_loop(runtime_.event_loop(), handle_);
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
