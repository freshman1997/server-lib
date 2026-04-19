#ifndef __YUAN_NET_ASYNC_ASYNC_CONNECTION_CONTEXT_H__
#define __YUAN_NET_ASYNC_ASYNC_CONNECTION_CONTEXT_H__

#include "buffer/byte_buffer.h"
#include "coroutine/runtime.h"
#include "coroutine/task.h"
#include "coroutine/stream_io_awaitable.h"
#include "coroutine/datagram_io_awaitable.h"
#include "net/connection/connection.h"
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
            : conn_(conn, [](Connection *) {}),
              runtime_(runtime)
        {
        }

        AsyncConnectionContext(std::shared_ptr<Connection> conn, coroutine::RuntimeView runtime)
            : conn_(conn), runtime_(runtime)
        {
        }

        ~AsyncConnectionContext() = default;

        AsyncConnectionContext(const AsyncConnectionContext &) = delete;
        AsyncConnectionContext &operator=(const AsyncConnectionContext &) = delete;

        AsyncConnectionContext(AsyncConnectionContext &&other) noexcept
            : conn_(other.conn_),
              runtime_(other.runtime_),
              closed_(other.closed_),
              default_handler_owner_(other.default_handler_owner_)
        {
            other.conn_ = nullptr;
            other.closed_ = true;
        }

        AsyncConnectionContext &operator=(AsyncConnectionContext &&other) noexcept
        {
            if (this != &other) {
                conn_ = other.conn_;
                runtime_ = other.runtime_;
                closed_ = other.closed_;
                default_handler_owner_ = other.default_handler_owner_;
                other.conn_ = nullptr;
                other.closed_ = true;
            }
            return *this;
        }

        void install_default_handler()
        {
            if (conn_) {
                conn_->set_connection_handler(default_handler_owner_);
            }
        }

        coroutine::Task<coroutine::ReadResult> read_async(uint32_t timeout_ms = 0)
        {
            if (!conn_ || closed_) {
                co_return coroutine::ReadResult::with_status(coroutine::IoStatus::invalid_state);
            }
            co_return co_await runtime_.read(conn_, timeout_ms);
        }

        coroutine::Task<coroutine::WriteResult> write_async(const ::yuan::buffer::ByteBuffer &buffer, uint32_t timeout_ms = 0)
        {
            if (!conn_ || closed_) {
                co_return coroutine::WriteResult{ coroutine::IoStatus::invalid_state };
            }
            co_return co_await runtime_.write(conn_, buffer, timeout_ms);
        }

        coroutine::Task<coroutine::WriteResult> flush_async(uint32_t timeout_ms = 0)
        {
            if (!conn_ || closed_) {
                co_return coroutine::WriteResult{ coroutine::IoStatus::invalid_state };
            }
            co_return co_await runtime_.flush(conn_, timeout_ms);
        }

        coroutine::Task<coroutine::IoStatus> close_async()
        {
            if (!conn_ || closed_) {
                co_return coroutine::IoStatus::invalid_state;
            }
            closed_ = true;
            co_return co_await runtime_.close(conn_);
        }

        coroutine::Task<coroutine::SslHandshakeResult> ssl_handshake_async(uint32_t timeout_ms = 0)
        {
            if (!conn_ || closed_) {
                co_return coroutine::SslHandshakeResult::invalid_state;
            }
            co_return co_await runtime_.ssl_handshake(conn_, timeout_ms);
        }

        void write(const ::yuan::buffer::ByteBuffer &buffer)
        {
            if (conn_) {
                conn_->write(buffer);
            }
        }

        void write_and_flush(const ::yuan::buffer::ByteBuffer &buffer)
        {
            if (conn_) {
                conn_->write_and_flush(buffer);
            }
        }

        void append_output(std::string_view text)
        {
            if (conn_) {
                conn_->append_output(text);
            }
        }

        void append_output(const char *data, std::size_t size)
        {
            if (conn_) {
                conn_->append_output(data, size);
            }
        }

        void append_output(const ::yuan::buffer::ByteBuffer &buffer)
        {
            if (conn_) {
                conn_->append_output(buffer);
            }
        }

        void flush()
        {
            if (conn_) {
                conn_->flush();
            }
        }

        void close()
        {
            if (conn_ && !closed_) {
                closed_ = true;
                conn_->close();
            }
        }

        void abort()
        {
            if (conn_ && !closed_) {
                closed_ = true;
                conn_->abort();
            }
        }

        ::yuan::buffer::ByteBuffer take_input_byte_buffer()
        {
            return conn_ ? conn_->take_input_byte_buffer() : ::yuan::buffer::ByteBuffer{};
        }

        ::yuan::buffer::ByteBuffer get_input_byte_buffer() const
        {
            return conn_ ? conn_->get_input_byte_buffer() : ::yuan::buffer::ByteBuffer{};
        }

        size_t input_readable_bytes() const
        {
            return conn_ ? conn_->input_readable_bytes() : 0;
        }

        bool is_connected() const
        {
            return conn_ && conn_->is_connected();
        }

        bool is_closed() const noexcept
        {
            return closed_;
        }

        const InetAddress &get_remote_address() const
        {
            return conn_->get_remote_address();
        }

        ConnectionState get_connection_state() const
        {
            return conn_ ? conn_->get_connection_state() : ConnectionState::closed;
        }

        uintptr_t connection_id() const
        {
            return reinterpret_cast<uintptr_t>(conn_.get());
        }

        Connection *native_handle() const noexcept
        {
            return conn_.get();
        }

        std::shared_ptr<Connection> connection() const noexcept
        {
            return conn_;
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
            return conn_ != nullptr && !closed_;
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
        };

        std::shared_ptr<Connection> conn_;
        coroutine::RuntimeView runtime_{};
        bool closed_ = false;
        std::shared_ptr<DefaultHandler> default_handler_owner_ = std::make_shared<DefaultHandler>();
    };

} // namespace yuan::net

#endif
