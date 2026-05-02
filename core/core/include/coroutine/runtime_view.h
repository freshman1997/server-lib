#ifndef __YUAN_COROUTINE_RUNTIME_VIEW_H__
#define __YUAN_COROUTINE_RUNTIME_VIEW_H__

#include <cstdint>
#include <functional>
#include <memory>

#include "buffer/byte_buffer.h"
#include "coroutine/scheduler.h"
#include "coroutine/event_loop_timeout_awaitable.h"
#include "coroutine/queue_in_loop_awaitable.h"
#include "timer/timer_util.hpp"

namespace yuan::timer
{
    class TimerManager;
    class Timer;
}

namespace yuan::net
{
    class Channel;
    class Connection;
    class ConnectionHandle;
    class ConnectionHandler;
}

namespace yuan::coroutine
{

    class AsyncReadAwaiter;
    class AsyncWriteAwaiter;
    class AsyncFlushAwaiter;
    class AsyncCloseAwaiter;
    class AsyncSslHandshakeAwaiter;
    class AsyncReceiveFromAwaiter;

    class RuntimeView
    {
    public:
        RuntimeView() = default;

        RuntimeView(net::EventLoop *event_loop, timer::TimerManager *timer_manager) noexcept
            : event_loop_(event_loop),
              timer_manager_(timer_manager)
        {
        }

        net::EventLoop *event_loop() const noexcept
        {
            return event_loop_;
        }

        timer::TimerManager *timer_manager() const noexcept
        {
            return timer_manager_;
        }

        EventLoopScheduler scheduler() const noexcept
        {
            return EventLoopScheduler(event_loop_);
        }

        void request_resume() const noexcept
        {
            if (event_loop_) {
                event_loop_->request_coroutine_resume();
            }
        }

        ScheduledQueueInLoopAwaiter dispatch_in_loop(std::function<void()> callback = {}) const
        {
            return dispatch_in_event_loop(event_loop_, std::move(callback));
        }

        ScheduleAwaiter schedule() const noexcept
        {
            scheduler_ = EventLoopScheduler(event_loop_);
            return schedule_on(&scheduler_);
        }

        ScheduledTimeoutAwaiter sleep_for(uint32_t timeout_ms) const noexcept
        {
            return sleep_in_event_loop(event_loop_, timer_manager_, timeout_ms);
        }

        AsyncReadAwaiter read(net::Connection *conn, uint32_t timeout_ms = 0) const noexcept;
        AsyncReadAwaiter read(const std::shared_ptr<net::Connection> &conn, uint32_t timeout_ms = 0) const noexcept;
        AsyncReadAwaiter read(const net::ConnectionHandle &conn, uint32_t timeout_ms = 0) const noexcept;
        AsyncReadAwaiter read(net::Connection *conn, uint32_t timeout_ms,
                              bool forward_terminal_events_after_completion) const noexcept;
        AsyncReadAwaiter read(const std::shared_ptr<net::Connection> &conn, uint32_t timeout_ms,
                              bool forward_terminal_events_after_completion) const noexcept;
        AsyncReadAwaiter read(const net::ConnectionHandle &conn, uint32_t timeout_ms,
                              bool forward_terminal_events_after_completion) const noexcept;
        AsyncWriteAwaiter write(net::Connection *conn, const ::yuan::buffer::ByteBuffer &buf,
                                uint32_t timeout_ms = 0) const noexcept;
        AsyncWriteAwaiter write(const std::shared_ptr<net::Connection> &conn, const ::yuan::buffer::ByteBuffer &buf,
                                uint32_t timeout_ms = 0) const noexcept;
        AsyncWriteAwaiter write(const net::ConnectionHandle &conn, const ::yuan::buffer::ByteBuffer &buf,
                                uint32_t timeout_ms = 0) const noexcept;
        AsyncFlushAwaiter flush(net::Connection *conn, uint32_t timeout_ms = 0) const noexcept;
        AsyncFlushAwaiter flush(const std::shared_ptr<net::Connection> &conn, uint32_t timeout_ms = 0) const noexcept;
        AsyncFlushAwaiter flush(const net::ConnectionHandle &conn, uint32_t timeout_ms = 0) const noexcept;
        AsyncCloseAwaiter close(net::Connection *conn) const noexcept;
        AsyncCloseAwaiter close(const std::shared_ptr<net::Connection> &conn) const noexcept;
        AsyncCloseAwaiter close(const net::ConnectionHandle &conn) const noexcept;
        AsyncSslHandshakeAwaiter ssl_handshake(net::Connection *conn, uint32_t timeout_ms = 0) const noexcept;
        AsyncSslHandshakeAwaiter ssl_handshake(const std::shared_ptr<net::Connection> &conn, uint32_t timeout_ms = 0) const noexcept;
        AsyncSslHandshakeAwaiter ssl_handshake(const net::ConnectionHandle &conn, uint32_t timeout_ms = 0) const noexcept;
        AsyncReceiveFromAwaiter receive_from(net::Connection *conn, uint32_t timeout_ms = 0) const noexcept;
        AsyncReceiveFromAwaiter receive_from(const std::shared_ptr<net::Connection> &conn, uint32_t timeout_ms = 0) const noexcept;
        AsyncReceiveFromAwaiter receive_from(const net::ConnectionHandle &conn, uint32_t timeout_ms = 0) const noexcept;

        timer::Timer *schedule(uint32_t delay_ms, std::function<void()> callback) const
        {
            if (!timer_manager_ || !callback) {
                return nullptr;
            }
            return timer::TimerUtil::build_timeout_timer(timer_manager_, delay_ms,
                                                         [cb = std::move(callback)](timer::Timer *) { cb(); });
        }

        timer::Timer *schedule_periodic(uint32_t delay_ms, uint32_t interval_ms,
                                        std::function<void()> callback, int repeat = 0) const
        {
            if (!timer_manager_ || !callback) {
                return nullptr;
            }
            return timer::TimerUtil::build_period_timer(timer_manager_, delay_ms, interval_ms,
                                                        [cb = std::move(callback)](timer::Timer *) { cb(); }, repeat);
        }

        static void cancel_timer(timer::Timer *timer)
        {
            if (timer) {
                timer->cancel();
            }
        }

        void register_connection(net::Connection *conn, std::shared_ptr<net::ConnectionHandler> handler) const;
        void register_connection(const std::shared_ptr<net::Connection> &conn, std::shared_ptr<net::ConnectionHandler> handler) const;
        void update_channel(net::Channel *channel) const;

    private:
        net::EventLoop *event_loop_ = nullptr;
        timer::TimerManager *timer_manager_ = nullptr;
        mutable EventLoopScheduler scheduler_{};
    };

} // namespace yuan::coroutine

#endif
