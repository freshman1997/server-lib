#ifndef __YUAN_NET_ASYNC_ASYNC_CLIENT_SESSION_H__
#define __YUAN_NET_ASYNC_ASYNC_CLIENT_SESSION_H__

#include <functional>
#include <memory>
#include <string>

#include "coroutine/connect_awaitable.h"
#include "coroutine/io_result.h"
#include "coroutine/runtime.h"
#include "coroutine/stream_io_awaitable.h"
#include "coroutine/task.h"
#include "net/async/async_connection_context.h"
#include "net/connection/connection.h"
#include "net/runtime/network_runtime.h"
#include "timer/timer.h"

namespace yuan::net
{

    class AsyncClientSession
    {
    public:
        AsyncClientSession() = default;

        explicit AsyncClientSession(Connection *conn, coroutine::RuntimeView runtime)
            : ctx_(conn, runtime)
        {
        }

        ~AsyncClientSession() = default;

        AsyncClientSession(const AsyncClientSession &) = delete;
        AsyncClientSession &operator=(const AsyncClientSession &) = delete;

        AsyncClientSession(AsyncClientSession &&) noexcept = default;
        AsyncClientSession &operator=(AsyncClientSession &&) noexcept = default;

        coroutine::Task<bool> connect_async(coroutine::RuntimeView runtime,
                                            const std::string &host,
                                            uint16_t port,
                                            uint32_t timeout_ms = 0)
        {
            auto result = co_await coroutine::async_connect(runtime, host, port, timeout_ms);
            if (result.result != coroutine::ConnectResult::success || !result.connection) {
                co_return false;
            }

            ctx_ = AsyncConnectionContext(result.connection, runtime);
            co_return true;
        }

        coroutine::Task<coroutine::ReadResult> read_async(uint32_t timeout_ms = 0)
        {
            auto result = co_await ctx_.read_async(timeout_ms);
            co_return result;
        }

        coroutine::Task<coroutine::WriteResult> write_async(const ::yuan::buffer::ByteBuffer &buffer,
                                                            uint32_t timeout_ms = 0)
        {
            auto result = co_await ctx_.write_async(buffer, timeout_ms);
            co_return result;
        }

        coroutine::Task<coroutine::WriteResult> flush_async(uint32_t timeout_ms = 0)
        {
            auto result = co_await ctx_.flush_async(timeout_ms);
            co_return result;
        }

        coroutine::Task<coroutine::IoStatus> close_async()
        {
            auto result = co_await ctx_.close_async();
            co_return result;
        }

        void write(const ::yuan::buffer::ByteBuffer &buffer)
        {
            ctx_.write(buffer);
        }

        void write_and_flush(const ::yuan::buffer::ByteBuffer &buffer)
        {
            ctx_.write_and_flush(buffer);
        }

        void close()
        {
            ctx_.close();
        }

        bool is_connected() const noexcept
        {
            return ctx_.is_connected();
        }

        AsyncConnectionContext &context() noexcept
        {
            return ctx_;
        }

        const AsyncConnectionContext &context() const noexcept
        {
            return ctx_;
        }

        Connection *native_handle() const noexcept
        {
            return ctx_.native_handle();
        }

        coroutine::RuntimeView runtime_view() const noexcept
        {
            return ctx_.runtime_view();
        }

        timer::Timer *schedule(uint32_t delay_ms, std::function<void()> callback)
        {
            return ctx_.schedule(delay_ms, std::move(callback));
        }

        timer::Timer *schedule_periodic(uint32_t delay_ms, uint32_t interval_ms,
                                        std::function<void()> callback, int repeat = 0)
        {
            return ctx_.schedule_periodic(delay_ms, interval_ms, std::move(callback), repeat);
        }

        void cancel_timer(timer::Timer *timer)
        {
            ctx_.cancel_timer(timer);
        }

        explicit operator bool() const noexcept
        {
            return static_cast<bool>(ctx_);
        }

    private:
        AsyncConnectionContext ctx_;
    };

} // namespace yuan::net

#endif
