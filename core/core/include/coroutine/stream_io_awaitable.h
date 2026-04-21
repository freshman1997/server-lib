#ifndef __YUAN_COROUTINE_STREAM_IO_AWAITABLE_H__
#define __YUAN_COROUTINE_STREAM_IO_AWAITABLE_H__

#include <coroutine>
#include <memory>

#include "coroutine/io_result.h"
#include "coroutine/runtime_view.h"
#include "net/connection/connection.h"
#include "net/connection/connection_ref.h"
#include "net/handler/connection_handler.h"
#include "net/secuity/ssl_handler.h"
#include "timer/timer_manager.h"
#include "timer/timer_util.hpp"

namespace yuan::coroutine
{

    class AsyncReadAwaiter
    {
    public:
        AsyncReadAwaiter(RuntimeView runtime, net::Connection *connection,
                         uint32_t timeout_ms = 0,
                         bool forward_terminal_events_after_completion = true) noexcept
            : runtime_(runtime),
              connection_ref_(connection),
              timeout_ms_(timeout_ms),
              forward_terminal_events_after_completion_(forward_terminal_events_after_completion)
        {
        }

        AsyncReadAwaiter(RuntimeView runtime,
                         std::shared_ptr<net::Connection> connection,
                         uint32_t timeout_ms = 0,
                         bool forward_terminal_events_after_completion = true) noexcept
            : runtime_(runtime),
              connection_ref_(std::move(connection)),
              timeout_ms_(timeout_ms),
              forward_terminal_events_after_completion_(forward_terminal_events_after_completion)
        {
        }

        bool await_ready() const noexcept
        {
            auto *connection = connection_ref_.get();
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
            auto *connection = connection_ref_.get();
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

            handle_ = handle;
            proxy_ = std::make_shared<ProxyHandler>(*this, connection,
                                                    connection->get_connection_handler(),
                                                    connection->get_connection_handler_owner());
            connection->set_connection_handler(proxy_);

            if (connection->input_readable_bytes() > 0) {
                result_.status = IoStatus::success;
                restore_handler_if_needed();
                proxy_.reset();
                return false;
            }

            if (connection->input_shutdown()) {
                result_.status = IoStatus::connection_closed;
                restore_handler_if_needed();
                proxy_.reset();
                return false;
            }

            if (connection->get_connection_state() != net::ConnectionState::connected) {
                result_.status = IoStatus::connection_closed;
                restore_handler_if_needed();
                proxy_.reset();
                return false;
            }

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

            auto *connection = connection_ref_.get();
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
            restore_handler_if_needed();
            if (runtime_.event_loop()) {
                runtime_.event_loop()->post_coroutine(handle_);
            }
        }

        class ProxyHandler final : public net::ConnectionHandler
        {
        public:
            ProxyHandler(AsyncReadAwaiter &owner, net::Connection *connection,
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
                if (next_ && owner_.forward_terminal_events_after_completion_) {
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
                    if (connection_ && connection_->input_readable_bytes() > 0) {
                        owner_.complete(IoStatus::success);
                    } else {
                        owner_.complete(IoStatus::connection_closed);
                    }
                }
                if (next_) {
                    next_->on_close(conn);
                }
            }

            void on_input_shutdown(const std::shared_ptr<net::Connection> &conn) override
            {
                if (!owner_.completed_) {
                    if (connection_ && connection_->input_readable_bytes() > 0) {
                        owner_.complete(IoStatus::success);
                    } else {
                        owner_.complete(IoStatus::connection_closed);
                    }
                }
                if (next_ && owner_.forward_terminal_events_after_completion_) {
                    next_->on_input_shutdown(conn);
                }
            }

            bool is_input_shutdown() const override
            {
                return next_ ? next_->is_input_shutdown() : false;
            }

            AsyncReadAwaiter &owner_;
            net::Connection *connection_;
            net::ConnectionHandler *next_;
            std::shared_ptr<net::ConnectionHandler> next_owner_;
        };

        void restore_handler_if_needed() noexcept
        {
            auto *connection = connection_ref_.get();
            if (handler_restored_ || !proxy_ || !connection) {
                return;
            }
            if (connection->get_connection_handler_owner() == proxy_) {
                connection->set_connection_handler(proxy_->next_owner_);
            }
            handler_restored_ = true;
        }

        RuntimeView runtime_{};
        net::ConnectionRef connection_ref_{};
        uint32_t timeout_ms_ = 0;
        timer::Timer *timeout_timer_ = nullptr;

        std::coroutine_handle<> handle_{};
        std::shared_ptr<ProxyHandler> proxy_;
        ReadResult result_{};
        bool completed_ = false;
        bool timed_out_ = false;
        bool handler_restored_ = false;
        bool forward_terminal_events_after_completion_ = true;

    public:
        ~AsyncReadAwaiter()
        {
            if (timeout_timer_) {
                timeout_timer_->cancel();
                timeout_timer_ = nullptr;
            }
        }
    };

    inline AsyncReadAwaiter async_read(
        RuntimeView runtime,
        net::Connection * connection,
        uint32_t timeout_ms = 0) noexcept
    {
        return AsyncReadAwaiter(runtime, connection, timeout_ms);
    }

    inline AsyncReadAwaiter async_read(
        RuntimeView runtime,
        net::Connection * connection,
        uint32_t timeout_ms,
        bool forward_terminal_events_after_completion) noexcept
    {
        return AsyncReadAwaiter(runtime, connection, timeout_ms, forward_terminal_events_after_completion);
    }

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
        bool forward_terminal_events_after_completion) noexcept
    {
        return AsyncReadAwaiter(runtime, connection, timeout_ms, forward_terminal_events_after_completion);
    }

    class AsyncWriteAwaiter
    {
    public:
        AsyncWriteAwaiter(RuntimeView runtime, net::Connection *connection,
                          ::yuan::buffer::ByteBuffer buffer,
                          uint32_t timeout_ms = 0) noexcept
            : runtime_(runtime),
              connection_ref_(connection),
              buffer_(std::move(buffer)),
              timeout_ms_(timeout_ms)
        {
        }

        AsyncWriteAwaiter(RuntimeView runtime,
                          std::shared_ptr<net::Connection> connection,
                          ::yuan::buffer::ByteBuffer buffer,
                          uint32_t timeout_ms = 0) noexcept
            : runtime_(runtime),
              connection_ref_(std::move(connection)),
              buffer_(std::move(buffer)),
              timeout_ms_(timeout_ms)
        {
        }

        bool await_ready() const noexcept
        {
            auto *connection = connection_ref_.get();
            return !connection || !runtime_.event_loop() ||
                   connection->get_connection_state() == net::ConnectionState::closed;
        }

        bool await_suspend(std::coroutine_handle<> handle)
        {
            auto *connection = connection_ref_.get();
            if (!connection || !runtime_.event_loop()) {
                result_.status = IoStatus::invalid_state;
                return false;
            }

            if (connection->get_connection_state() == net::ConnectionState::closed) {
                result_.status = IoStatus::connection_closed;
                return false;
            }

            handle_ = handle;
            proxy_ = std::make_shared<ProxyHandler>(*this, connection,
                                                    connection->get_connection_handler(),
                                                    connection->get_connection_handler_owner());
            connection->set_connection_handler(proxy_);
            connection->write_and_flush(buffer_);

            if (completed_) {
                return true;
            }
            if (connection->output_readable_bytes() == 0) {
                result_.status = IoStatus::success;
                restore_handler_if_needed();
                proxy_.reset();
                return false;
            }

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

        WriteResult await_resume() noexcept
        {
            restore_handler_if_needed();
            proxy_.reset();

            if (timeout_timer_) {
                timeout_timer_->cancel();
                timeout_timer_ = nullptr;
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
            ProxyHandler(AsyncWriteAwaiter &owner, net::Connection *connection,
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
                if (next_) {
                    next_->on_read(conn);
                }
            }

            void on_write(const std::shared_ptr<net::Connection> &conn) override
            {
                if (!owner_.completed_) {
                    if (connection_ && connection_->output_readable_bytes() == 0) {
                        owner_.complete(IoStatus::success);
                        return;
                    }
                }
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

            void on_input_shutdown(const std::shared_ptr<net::Connection> &conn) override
            {
                if (!owner_.completed_) {
                    owner_.complete(IoStatus::connection_closed);
                    return;
                }
                if (next_) {
                    next_->on_input_shutdown(conn);
                }
            }

            AsyncWriteAwaiter &owner_;
            net::Connection *connection_;
            net::ConnectionHandler *next_;
            std::shared_ptr<net::ConnectionHandler> next_owner_;
        };

        void restore_handler_if_needed() noexcept
        {
            auto *connection = connection_ref_.get();
            if (handler_restored_ || !proxy_ || !connection) {
                return;
            }
            if (connection->get_connection_handler_owner() == proxy_) {
                connection->set_connection_handler(proxy_->next_owner_);
            }
            handler_restored_ = true;
        }

        RuntimeView runtime_{};
        net::ConnectionRef connection_ref_{};
        ::yuan::buffer::ByteBuffer buffer_;
        uint32_t timeout_ms_ = 0;
        timer::Timer *timeout_timer_ = nullptr;

        std::coroutine_handle<> handle_{};
        std::shared_ptr<ProxyHandler> proxy_;
        WriteResult result_{};
        bool completed_ = false;
        bool timed_out_ = false;
        bool handler_restored_ = false;

    public:
        ~AsyncWriteAwaiter()
        {
            if (timeout_timer_) {
                timeout_timer_->cancel();
                timeout_timer_ = nullptr;
            }
        }
    };

    inline AsyncWriteAwaiter async_write(
        RuntimeView runtime,
        net::Connection * connection,
        const ::yuan::buffer::ByteBuffer & buffer,
        uint32_t timeout_ms = 0) noexcept
    {
        return AsyncWriteAwaiter(runtime, connection, buffer.copy_readable(), timeout_ms);
    }

    inline AsyncWriteAwaiter async_write(
        RuntimeView runtime,
        const std::shared_ptr<net::Connection> &connection,
        const ::yuan::buffer::ByteBuffer & buffer,
        uint32_t timeout_ms = 0) noexcept
    {
        return AsyncWriteAwaiter(runtime, connection, buffer.copy_readable(), timeout_ms);
    }

    class AsyncFlushAwaiter
    {
    public:
        AsyncFlushAwaiter(RuntimeView runtime, net::Connection *connection,
                          uint32_t timeout_ms = 0) noexcept
            : runtime_(runtime),
              connection_ref_(connection),
              timeout_ms_(timeout_ms)
        {
        }

        AsyncFlushAwaiter(RuntimeView runtime,
                          std::shared_ptr<net::Connection> connection,
                          uint32_t timeout_ms = 0) noexcept
            : runtime_(runtime),
              connection_ref_(std::move(connection)),
              timeout_ms_(timeout_ms)
        {
        }

        bool await_ready() const noexcept
        {
            auto *connection = connection_ref_.get();
            return !connection || !runtime_.event_loop() ||
                   connection->get_connection_state() == net::ConnectionState::closed;
        }

        bool await_suspend(std::coroutine_handle<> handle)
        {
            auto *connection = connection_ref_.get();
            if (!connection || !runtime_.event_loop()) {
                result_.status = IoStatus::invalid_state;
                return false;
            }

            if (connection->get_connection_state() == net::ConnectionState::closed) {
                result_.status = IoStatus::connection_closed;
                return false;
            }

            handle_ = handle;
            proxy_ = std::make_shared<ProxyHandler>(*this, connection,
                                                    connection->get_connection_handler(),
                                                    connection->get_connection_handler_owner());
            connection->set_connection_handler(proxy_);
            connection->flush();

            if (completed_) {
                return true;
            }
            if (connection->output_readable_bytes() == 0) {
                result_.status = IoStatus::success;
                restore_handler_if_needed();
                proxy_.reset();
                return false;
            }

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

        WriteResult await_resume() noexcept
        {
            restore_handler_if_needed();
            proxy_.reset();

            if (timeout_timer_) {
                timeout_timer_->cancel();
                timeout_timer_ = nullptr;
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
            ProxyHandler(AsyncFlushAwaiter &owner, net::Connection *connection,
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
                if (next_) {
                    next_->on_read(conn);
                }
            }

            void on_write(const std::shared_ptr<net::Connection> &conn) override
            {
                if (!owner_.completed_) {
                    if (connection_ && connection_->output_readable_bytes() == 0) {
                        owner_.complete(IoStatus::success);
                        return;
                    }
                }
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

            void on_input_shutdown(const std::shared_ptr<net::Connection> &conn) override
            {
                if (!owner_.completed_) {
                    owner_.complete(IoStatus::connection_closed);
                    return;
                }
                if (next_) {
                    next_->on_input_shutdown(conn);
                }
            }

            AsyncFlushAwaiter &owner_;
            net::Connection *connection_;
            net::ConnectionHandler *next_;
            std::shared_ptr<net::ConnectionHandler> next_owner_;
        };

        void restore_handler_if_needed() noexcept
        {
            auto *connection = connection_ref_.get();
            if (handler_restored_ || !proxy_ || !connection) {
                return;
            }
            if (connection->get_connection_handler_owner() == proxy_) {
                connection->set_connection_handler(proxy_->next_owner_);
            }
            handler_restored_ = true;
        }

        RuntimeView runtime_{};
        net::ConnectionRef connection_ref_{};
        uint32_t timeout_ms_ = 0;
        timer::Timer *timeout_timer_ = nullptr;

        std::coroutine_handle<> handle_{};
        std::shared_ptr<ProxyHandler> proxy_;
        WriteResult result_{};
        bool completed_ = false;
        bool timed_out_ = false;
        bool handler_restored_ = false;

    public:
        ~AsyncFlushAwaiter()
        {
            if (timeout_timer_) {
                timeout_timer_->cancel();
                timeout_timer_ = nullptr;
            }
        }
    };

    inline AsyncFlushAwaiter async_flush(
        RuntimeView runtime,
        net::Connection * connection,
        uint32_t timeout_ms = 0) noexcept
    {
        return AsyncFlushAwaiter(runtime, connection, timeout_ms);
    }

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
        AsyncCloseAwaiter(RuntimeView runtime, net::Connection *connection) noexcept
            : runtime_(runtime),
              connection_ref_(connection)
        {
        }

        AsyncCloseAwaiter(RuntimeView runtime, std::shared_ptr<net::Connection> connection) noexcept
            : runtime_(runtime),
              connection_ref_(std::move(connection))
        {
        }

        bool await_ready() const noexcept
        {
            auto *connection = connection_ref_.get();
            if (!connection || !runtime_.event_loop()) {
                return true;
            }
            return connection->get_connection_state() == net::ConnectionState::closed;
        }

        bool await_suspend(std::coroutine_handle<> handle)
        {
            auto *connection = connection_ref_.get();
            if (!connection || !runtime_.event_loop()) {
                result_ = IoStatus::invalid_state;
                return false;
            }

            handle_ = handle;
            proxy_ = std::make_shared<ProxyHandler>(*this, connection,
                                                    connection->get_connection_handler(),
                                                    connection->get_connection_handler_owner());
            connection->set_connection_handler(proxy_);
            connection->close();

            if (completed_) {
                return true;
            }
            if (connection->get_connection_state() == net::ConnectionState::closed) {
                result_ = IoStatus::success;
                restore_handler_if_needed();
                proxy_.reset();
                return false;
            }
            return true;
        }

        IoStatus await_resume() noexcept
        {
            restore_handler_if_needed();
            proxy_.reset();
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

        class ProxyHandler final : public net::ConnectionHandler
        {
        public:
            ProxyHandler(AsyncCloseAwaiter &owner, net::Connection *connection,
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
                    owner_.complete(IoStatus::success);
                    return;
                }
                if (next_) {
                    next_->on_close(conn);
                }
            }

            void on_input_shutdown(const std::shared_ptr<net::Connection> &conn) override
            {
                if (!owner_.completed_) {
                    owner_.complete(IoStatus::connection_closed);
                    return;
                }
                if (next_) {
                    next_->on_input_shutdown(conn);
                }
            }

            AsyncCloseAwaiter &owner_;
            net::Connection *connection_;
            net::ConnectionHandler *next_;
            std::shared_ptr<net::ConnectionHandler> next_owner_;
        };

        void restore_handler_if_needed() noexcept
        {
            auto *connection = connection_ref_.get();
            if (handler_restored_ || !proxy_ || !connection) {
                return;
            }
            if (connection->get_connection_handler_owner() == proxy_) {
                connection->set_connection_handler(proxy_->next_owner_);
            }
            handler_restored_ = true;
        }

        RuntimeView runtime_{};
        net::ConnectionRef connection_ref_{};

        std::coroutine_handle<> handle_{};
        std::shared_ptr<ProxyHandler> proxy_;
        IoStatus result_ = IoStatus::success;
        bool completed_ = false;
        bool handler_restored_ = false;
    };

    inline AsyncCloseAwaiter async_close(
        RuntimeView runtime,
        net::Connection * connection) noexcept
    {
        return AsyncCloseAwaiter(runtime, connection);
    }

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
        AsyncSslHandshakeAwaiter(RuntimeView runtime, net::Connection *connection,
                                 uint32_t timeout_ms = 0) noexcept
            : runtime_(runtime),
              connection_ref_(connection),
              timeout_ms_(timeout_ms)
        {
        }

        AsyncSslHandshakeAwaiter(RuntimeView runtime,
                                 std::shared_ptr<net::Connection> connection,
                                 uint32_t timeout_ms = 0) noexcept
            : runtime_(runtime),
              connection_ref_(std::move(connection)),
              timeout_ms_(timeout_ms)
        {
        }

        bool await_ready() const noexcept
        {
            auto *connection = connection_ref_.get();
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
            auto *connection = connection_ref_.get();
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
                if (timeout_timer_) {
                    timeout_timer_->cancel();
                    timeout_timer_ = nullptr;
                }
                if (runtime_.event_loop()) {
                    runtime_.event_loop()->post_coroutine(handle_);
                }
            });

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
                        complete(SslHandshakeResult::timed_out);
                    });
            }

            return true;
        }

        SslHandshakeResult await_resume() noexcept
        {
            auto *connection = connection_ref_.get();
            if (connection) {
                connection->set_ssl_handshake_callback(nullptr);
            }

            if (timeout_timer_) {
                timeout_timer_->cancel();
                timeout_timer_ = nullptr;
            }

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
            auto *connection = connection_ref_.get();
            if (connection) {
                connection->set_ssl_handshake_callback(nullptr);
                connection->set_ssl_handshaking(false);
            }
            if (runtime_.event_loop()) {
                runtime_.event_loop()->post_coroutine(handle_);
            }
        }

        RuntimeView runtime_{};
        net::ConnectionRef connection_ref_{};
        uint32_t timeout_ms_ = 0;
        timer::Timer *timeout_timer_ = nullptr;

        std::coroutine_handle<> handle_{};
        mutable SslHandshakeResult result_ = SslHandshakeResult::failed;
        bool completed_ = false;
        bool timed_out_ = false;
    };

    inline AsyncSslHandshakeAwaiter async_ssl_handshake(
        RuntimeView runtime,
        net::Connection * connection,
        uint32_t timeout_ms = 0) noexcept
    {
        return AsyncSslHandshakeAwaiter(runtime, connection, timeout_ms);
    }

    inline AsyncSslHandshakeAwaiter async_ssl_handshake(
        RuntimeView runtime,
        const std::shared_ptr<net::Connection> &connection,
        uint32_t timeout_ms = 0) noexcept
    {
        return AsyncSslHandshakeAwaiter(runtime, connection, timeout_ms);
    }

} // namespace yuan::coroutine

#endif
