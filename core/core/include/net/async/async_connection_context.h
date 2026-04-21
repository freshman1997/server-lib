#ifndef __YUAN_NET_ASYNC_ASYNC_CONNECTION_CONTEXT_H__
#define __YUAN_NET_ASYNC_ASYNC_CONNECTION_CONTEXT_H__

#include "buffer/byte_buffer.h"
#include "coroutine/runtime.h"
#include "coroutine/task.h"
#include "coroutine/stream_io_awaitable.h"
#include "coroutine/datagram_io_awaitable.h"
#include "net/connection/connection.h"
#include "net/connection/connection_ref.h"
#include "net/handler/connection_handler.h"
#include "net/socket/inet_address.h"
#include "timer/timer.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace yuan::net
{

    class AsyncConnectionContext
    {
    public:
        AsyncConnectionContext() = default;

        AsyncConnectionContext(Connection *conn, coroutine::RuntimeView runtime)
            : conn_ref_(conn),
              runtime_(runtime)
        {
        }

        AsyncConnectionContext(std::shared_ptr<Connection> conn, coroutine::RuntimeView runtime)
            : conn_ref_(std::move(conn)), runtime_(runtime)
        {
        }

        ~AsyncConnectionContext() = default;

        AsyncConnectionContext(const AsyncConnectionContext &) = delete;
        AsyncConnectionContext &operator=(const AsyncConnectionContext &) = delete;

        AsyncConnectionContext(AsyncConnectionContext &&other) noexcept
            : conn_ref_(other.conn_ref_),
              runtime_(other.runtime_),
              closed_(other.closed_),
              default_handler_owner_(other.default_handler_owner_)
        {
            other.conn_ref_ = ConnectionRef{};
            other.closed_ = true;
        }

        AsyncConnectionContext &operator=(AsyncConnectionContext &&other) noexcept
        {
            if (this != &other) {
                conn_ref_ = other.conn_ref_;
                runtime_ = other.runtime_;
                closed_ = other.closed_;
                default_handler_owner_ = other.default_handler_owner_;
                other.conn_ref_ = ConnectionRef{};
                other.closed_ = true;
            }
            return *this;
        }

        void install_default_handler()
        {
            auto *conn = conn_ref_.get();
            if (conn) {
                conn->set_connection_handler(default_handler_owner_);
            }
        }

        coroutine::Task<coroutine::ReadResult> read_async(uint32_t timeout_ms = 0)
        {
            auto *conn = conn_ref_.get();
            if (!conn || closed_) {
                co_return coroutine::ReadResult::with_status(coroutine::IoStatus::invalid_state);
            }
            co_return co_await runtime_.read(conn, timeout_ms);
        }

        coroutine::Task<coroutine::ReadResult> read_async(
            uint32_t timeout_ms,
            bool forward_terminal_events_after_completion)
        {
            auto *conn = conn_ref_.get();
            if (!conn || closed_) {
                co_return coroutine::ReadResult::with_status(coroutine::IoStatus::invalid_state);
            }
            co_return co_await runtime_.read(conn, timeout_ms, forward_terminal_events_after_completion);
        }

        coroutine::Task<coroutine::WriteResult> write_async(const ::yuan::buffer::ByteBuffer &buffer, uint32_t timeout_ms = 0)
        {
            auto *conn = conn_ref_.get();
            if (!conn || closed_) {
                co_return coroutine::WriteResult{ coroutine::IoStatus::invalid_state };
            }
            co_return co_await runtime_.write(conn, buffer, timeout_ms);
        }

        coroutine::Task<coroutine::WriteResult> flush_async(uint32_t timeout_ms = 0)
        {
            auto *conn = conn_ref_.get();
            if (!conn || closed_) {
                co_return coroutine::WriteResult{ coroutine::IoStatus::invalid_state };
            }
            co_return co_await runtime_.flush(conn, timeout_ms);
        }

        coroutine::Task<coroutine::IoStatus> close_async()
        {
            auto *conn = conn_ref_.get();
            if (!conn || closed_) {
                co_return coroutine::IoStatus::invalid_state;
            }
            closed_ = true;
            co_return co_await runtime_.close(conn);
        }

        coroutine::Task<coroutine::SslHandshakeResult> ssl_handshake_async(uint32_t timeout_ms = 0)
        {
            auto *conn = conn_ref_.get();
            if (!conn || closed_) {
                co_return coroutine::SslHandshakeResult::invalid_state;
            }
            co_return co_await runtime_.ssl_handshake(conn, timeout_ms);
        }

        void write(const ::yuan::buffer::ByteBuffer &buffer)
        {
            auto *conn = conn_ref_.get();
            if (conn) {
                conn->write(buffer);
            }
        }

        void write_and_flush(const ::yuan::buffer::ByteBuffer &buffer)
        {
            auto *conn = conn_ref_.get();
            if (conn) {
                conn->write_and_flush(buffer);
            }
        }

        void append_output(std::string_view text)
        {
            auto *conn = conn_ref_.get();
            if (conn) {
                conn->append_output(text);
            }
        }

        void append_output(const char *data, std::size_t size)
        {
            auto *conn = conn_ref_.get();
            if (conn) {
                conn->append_output(data, size);
            }
        }

        void append_output(const ::yuan::buffer::ByteBuffer &buffer)
        {
            auto *conn = conn_ref_.get();
            if (conn) {
                conn->append_output(buffer);
            }
        }

        void flush()
        {
            auto *conn = conn_ref_.get();
            if (conn) {
                conn->flush();
            }
        }

        void close()
        {
            auto *conn = conn_ref_.get();
            if (conn && !closed_) {
                closed_ = true;
                conn->close();
            }
        }

        void abort()
        {
            auto *conn = conn_ref_.get();
            if (conn && !closed_) {
                closed_ = true;
                conn->abort();
            }
        }

        ::yuan::buffer::ByteBuffer take_input_byte_buffer()
        {
            auto *conn = conn_ref_.get();
            return conn ? conn->take_input_byte_buffer() : ::yuan::buffer::ByteBuffer{};
        }

        ::yuan::buffer::ByteBuffer get_input_byte_buffer() const
        {
            auto *conn = conn_ref_.get();
            return conn ? conn->get_input_byte_buffer() : ::yuan::buffer::ByteBuffer{};
        }

        size_t input_readable_bytes() const
        {
            auto *conn = conn_ref_.get();
            return conn ? conn->input_readable_bytes() : 0;
        }

        bool is_connected() const
        {
            auto *conn = conn_ref_.get();
            return conn && conn->is_connected();
        }

        bool is_closed() const noexcept
        {
            return closed_;
        }

        const InetAddress &get_remote_address() const
        {
            return conn_ref_->get_remote_address();
        }

        ConnectionState get_connection_state() const
        {
            auto *conn = conn_ref_.get();
            return conn ? conn->get_connection_state() : ConnectionState::closed;
        }

        uintptr_t connection_id() const
        {
            return reinterpret_cast<uintptr_t>(conn_ref_.get());
        }

        Connection *native_handle() const noexcept
        {
            return conn_ref_.get();
        }

        std::shared_ptr<Connection> connection() const noexcept
        {
            return conn_ref_.owner();
        }

        coroutine::RuntimeView runtime_view() const noexcept
        {
            return runtime_;
        }

        timer::Timer *schedule(uint32_t delay_ms, std::function<void()> callback)
        {
            return runtime_.schedule(delay_ms, std::move(callback));
        }

        timer::Timer *schedule_periodic(uint32_t delay_ms, uint32_t interval_ms,
                                        std::function<void()> callback, int repeat = 0)
        {
            return runtime_.schedule_periodic(delay_ms, interval_ms, std::move(callback), repeat);
        }

        void cancel_timer(timer::Timer *timer)
        {
            if (timer) {
                timer->cancel();
            }
        }

        explicit operator bool() const noexcept
        {
            return static_cast<bool>(conn_ref_) && !closed_;
        }

    private:
        class DefaultHandler final : public ConnectionHandler
        {
        public:
            void on_connected(const std::shared_ptr<Connection> &) override
            {
            }
            void on_error(const std::shared_ptr<Connection> &conn) override
            {
                if (conn) {
                    conn->close();
                }
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
            void on_input_shutdown(const std::shared_ptr<Connection> &conn) override
            {
                if (conn) {
                    conn->close();
                }
            }
        };

        ConnectionRef conn_ref_;
        coroutine::RuntimeView runtime_{};
        bool closed_ = false;
        std::shared_ptr<DefaultHandler> default_handler_owner_ = std::make_shared<DefaultHandler>();
    };

} // namespace yuan::net

#endif
