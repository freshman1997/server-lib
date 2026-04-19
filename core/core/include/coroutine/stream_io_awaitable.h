#ifndef __YUAN_COROUTINE_STREAM_IO_AWAITABLE_H__
#define __YUAN_COROUTINE_STREAM_IO_AWAITABLE_H__

#include <coroutine>
#include <memory>

#include "coroutine/io_result.h"
#include "coroutine/runtime_view.h"
#include "net/connection/connection.h"
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
            if (connection_->get_connection_state() == net::ConnectionState::closed) {
                return true;
            }
            return connection_->input_readable_bytes() > 0 || connection_->input_shutdown();
        }

        bool await_suspend(std::coroutine_handle<> handle)
        {
            if (!connection_ || !runtime_.event_loop()) {
                result_.status = IoStatus::invalid_state;
                return false;
            }

            if (connection_->get_connection_state() == net::ConnectionState::closed) {
                result_.status = IoStatus::connection_closed;
                return false;
            }

            if (connection_->input_readable_bytes() > 0) {
                return false;
            }

            if (connection_->input_shutdown()) {
                result_.status = IoStatus::connection_closed;
                return false;
            }

            handle_ = handle;
            proxy_ = std::make_shared<ProxyHandler>(*this, connection_,
                                                    connection_->get_connection_handler(),
                                                    connection_->get_connection_handler_owner());
            connection_->set_connection_handler(proxy_);

            if (connection_->input_readable_bytes() > 0) {
                result_.status = IoStatus::success;
                restore_handler_if_needed();
                proxy_.reset();
                return false;
            }

            if (connection_->input_shutdown()) {
                result_.status = IoStatus::connection_closed;
                restore_handler_if_needed();
                proxy_.reset();
                return false;
            }

            if (connection_->get_connection_state() != net::ConnectionState::connected) {
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
        const std::shared_ptr<net::Connection> &connection,
        uint32_t timeout_ms = 0) noexcept
    {
        return AsyncReadAwaiter(runtime, connection.get(), timeout_ms);
    }

    class AsyncWriteAwaiter
    {
    public:
        AsyncWriteAwaiter(RuntimeView runtime, net::Connection *connection,
                          ::yuan::buffer::ByteBuffer buffer,
                          uint32_t timeout_ms = 0) noexcept
            : runtime_(runtime),
              connection_(connection),
              buffer_(std::move(buffer)),
              timeout_ms_(timeout_ms)
        {
        }

        bool await_ready() const noexcept
        {
            return !connection_ || !runtime_.event_loop() ||
                   connection_->get_connection_state() == net::ConnectionState::closed;
        }

        bool await_suspend(std::coroutine_handle<> handle)
        {
            if (!connection_ || !runtime_.event_loop()) {
                result_.status = IoStatus::invalid_state;
                return false;
            }

            if (connection_->get_connection_state() == net::ConnectionState::closed) {
                result_.status = IoStatus::connection_closed;
                return false;
            }

            handle_ = handle;
            proxy_ = std::make_shared<ProxyHandler>(*this, connection_,
                                                    connection_->get_connection_handler(),
                                                    connection_->get_connection_handler_owner());
            connection_->set_connection_handler(proxy_);
            connection_->write_and_flush(buffer_);

            if (completed_) {
                return true;
            }
            if (connection_->output_readable_bytes() == 0) {
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
                    owner_.complete(IoStatus::success);
                    return;
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

            AsyncWriteAwaiter &owner_;
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
        return AsyncWriteAwaiter(runtime, connection.get(), buffer.copy_readable(), timeout_ms);
    }

    class AsyncFlushAwaiter
    {
    public:
        AsyncFlushAwaiter(RuntimeView runtime, net::Connection *connection,
                          uint32_t timeout_ms = 0) noexcept
            : runtime_(runtime),
              connection_(connection),
              timeout_ms_(timeout_ms)
        {
        }

        bool await_ready() const noexcept
        {
            return !connection_ || !runtime_.event_loop() ||
                   connection_->get_connection_state() == net::ConnectionState::closed;
        }

        bool await_suspend(std::coroutine_handle<> handle)
        {
            if (!connection_ || !runtime_.event_loop()) {
                result_.status = IoStatus::invalid_state;
                return false;
            }

            if (connection_->get_connection_state() == net::ConnectionState::closed) {
                result_.status = IoStatus::connection_closed;
                return false;
            }

            handle_ = handle;
            proxy_ = std::make_shared<ProxyHandler>(*this, connection_,
                                                    connection_->get_connection_handler(),
                                                    connection_->get_connection_handler_owner());
            connection_->set_connection_handler(proxy_);
            connection_->flush();

            if (completed_) {
                return true;
            }
            if (connection_->output_readable_bytes() == 0) {
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
                    owner_.complete(IoStatus::success);
                    return;
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

            AsyncFlushAwaiter &owner_;
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
        return AsyncFlushAwaiter(runtime, connection.get(), timeout_ms);
    }

    class AsyncCloseAwaiter
    {
    public:
        AsyncCloseAwaiter(RuntimeView runtime, net::Connection *connection) noexcept
            : runtime_(runtime),
              connection_(connection)
        {
        }

        bool await_ready() const noexcept
        {
            if (!connection_ || !runtime_.event_loop()) {
                return true;
            }
            return connection_->get_connection_state() == net::ConnectionState::closed;
        }

        bool await_suspend(std::coroutine_handle<> handle)
        {
            if (!connection_ || !runtime_.event_loop()) {
                result_ = IoStatus::invalid_state;
                return false;
            }

            handle_ = handle;
            proxy_ = std::make_shared<ProxyHandler>(*this, connection_,
                                                    connection_->get_connection_handler(),
                                                    connection_->get_connection_handler_owner());
            connection_->set_connection_handler(proxy_);
            connection_->close();

            if (completed_) {
                return true;
            }
            if (connection_->get_connection_state() == net::ConnectionState::closed) {
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

            AsyncCloseAwaiter &owner_;
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
        return AsyncCloseAwaiter(runtime, connection.get());
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
              connection_(connection),
              timeout_ms_(timeout_ms)
        {
        }

        bool await_ready() const noexcept
        {
            if (!connection_ || !runtime_.event_loop()) {
                result_ = SslHandshakeResult::invalid_state;
                return true;
            }
            auto ssl = connection_->get_ssl_handler();
            if (!ssl) {
                result_ = SslHandshakeResult::invalid_state;
                return true;
            }
            return false;
        }

        bool await_suspend(std::coroutine_handle<> handle)
        {
            if (!connection_ || !runtime_.event_loop()) {
                result_ = SslHandshakeResult::invalid_state;
                return false;
            }

            auto ssl = connection_->get_ssl_handler();
            if (!ssl) {
                result_ = SslHandshakeResult::invalid_state;
                return false;
            }

            handle_ = handle;

            if (!connection_->is_ssl_handshaking()) {
                int ret = ssl->ssl_init_action();
                if (ret > 0) {
                    result_ = SslHandshakeResult::success;
                    return false;
                }
                if (!ssl->ssl_want_read() && !ssl->ssl_want_write()) {
                    result_ = SslHandshakeResult::failed;
                    return false;
                }
                connection_->set_ssl_handshaking(true);
            }

            connection_->set_ssl_handshake_callback([this](bool success) {
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
            if (connection_) {
                connection_->set_ssl_handshake_callback(nullptr);
            }

            if (timeout_timer_) {
                timeout_timer_->cancel();
                timeout_timer_ = nullptr;
            }

            if (connection_) {
                connection_->set_ssl_handshaking(false);
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
            if (connection_) {
                connection_->set_ssl_handshake_callback(nullptr);
                connection_->set_ssl_handshaking(false);
            }
            if (runtime_.event_loop()) {
                runtime_.event_loop()->post_coroutine(handle_);
            }
        }

        RuntimeView runtime_{};
        net::Connection *connection_ = nullptr;
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
        return AsyncSslHandshakeAwaiter(runtime, connection.get(), timeout_ms);
    }

} // namespace yuan::coroutine

#endif
