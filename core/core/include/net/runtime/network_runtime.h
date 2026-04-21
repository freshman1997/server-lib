#ifndef __YUAN_NET_RUNTIME_NETWORK_RUNTIME_H__
#define __YUAN_NET_RUNTIME_NETWORK_RUNTIME_H__

#include <memory>
#include <functional>

#include "coroutine/runtime.h"
#include "coroutine/scheduler.h"
#include "coroutine/event_loop_timeout_awaitable.h"
#include "coroutine/queue_in_loop_awaitable.h"
#include "timer/timer.h"

namespace yuan::timer
{
    class TimerManager;
    class Timer;
}

namespace yuan::net
{

    class Channel;
    class ConnectionHandler;
    class Connection;
    class Connector;
    class ConnectorHandler;
    class Acceptor;
    class Poller;
    class EventLoop;
    enum class EventLoopExitReason;

    class NetworkRuntime
    {
    public:
        NetworkRuntime();

        NetworkRuntime(EventLoop *loop, timer::TimerManager *tm);

        ~NetworkRuntime();

        NetworkRuntime(const NetworkRuntime &) = delete;
        NetworkRuntime &operator=(const NetworkRuntime &) = delete;

        static Poller *create_default_poller();

        EventLoop *event_loop() const noexcept;
        timer::TimerManager *timer_manager() const noexcept;
        Poller *poller() const noexcept;
        bool owns_loop() const noexcept;

        class RuntimeView;
        RuntimeView runtime_view() const noexcept;

        EventLoopExitReason run();
        void stop();

        timer::Timer *schedule(uint32_t delay_ms, std::function<void()> callback);

        timer::Timer *schedule_periodic(uint32_t delay_ms, uint32_t interval_ms, std::function<void()> callback, int repeat = 0);

        void cancel_timer(timer::Timer *timer);

        void dispatch(std::function<void()> callback);

        void register_connection(const std::shared_ptr<Connection> &conn, std::shared_ptr<ConnectionHandler> handler);
        void register_connection(Connection *conn, std::shared_ptr<ConnectionHandler> handler);

        void register_connector(Connector *connector, std::shared_ptr<ConnectorHandler> handler);

        template<typename ConnectorT>
        void register_connector(const std::unique_ptr<ConnectorT> &connector,
                                std::shared_ptr<ConnectorHandler> handler)
        {
            register_connector(static_cast<Connector *>(connector ? &*connector : nullptr), std::move(handler));
        }

        void register_acceptor(Acceptor *acceptor, std::shared_ptr<ConnectionHandler> handler, Channel *channel = nullptr);

        template<typename AcceptorT>
        void register_acceptor(const std::unique_ptr<AcceptorT> &acceptor,
                               std::shared_ptr<ConnectionHandler> handler,
                               Channel *channel = nullptr)
        {
            register_acceptor(static_cast<Acceptor *>(acceptor ? &*acceptor : nullptr), std::move(handler), channel);
        }

        void update_channel(Channel *channel);

    private:
        std::unique_ptr<EventLoop> owned_loop_;
        std::unique_ptr<timer::TimerManager> owned_timer_manager_;
        std::unique_ptr<Poller> owned_poller_;

        EventLoop *loop_;
        timer::TimerManager *timer_manager_;
        Poller *poller_;
        bool owns_;
    };

    class NetworkRuntime::RuntimeView
    {
    public:
        RuntimeView() = default;
        RuntimeView(EventLoop *loop, timer::TimerManager *tm) noexcept
            : view_(loop, tm)
        {
        }

        explicit RuntimeView(const coroutine::RuntimeView &view) noexcept
            : view_(view)
        {
        }

        EventLoop *event_loop() const noexcept
        {
            return view_.event_loop();
        }
        timer::TimerManager *timer_manager() const noexcept
        {
            return view_.timer_manager();
        }

        operator coroutine::RuntimeView() const noexcept
        {
            return view_;
        }

        coroutine::RuntimeView raw() const noexcept
        {
            return view_;
        }

        coroutine::EventLoopScheduler scheduler() const noexcept
        {
            return view_.scheduler();
        }

        void request_resume() const noexcept
        {
            view_.request_resume();
        }

        coroutine::ScheduledQueueInLoopAwaiter dispatch_in_loop(std::function<void()> callback = {}) const
        {
            return view_.dispatch_in_loop(std::move(callback));
        }

        coroutine::ScheduleAwaiter schedule() const noexcept
        {
            return view_.schedule();
        }

        coroutine::ScheduledTimeoutAwaiter sleep_for(uint32_t timeout_ms) const noexcept
        {
            return view_.sleep_for(timeout_ms);
        }

        coroutine::AsyncReadAwaiter read(Connection *conn, uint32_t timeout_ms = 0) const noexcept
        {
            return view_.read(conn, timeout_ms);
        }

        coroutine::AsyncWriteAwaiter write(Connection *conn, const ::yuan::buffer::ByteBuffer &buf,
                                           uint32_t timeout_ms = 0) const noexcept
        {
            return view_.write(conn, buf, timeout_ms);
        }

        coroutine::AsyncFlushAwaiter flush(Connection *conn, uint32_t timeout_ms = 0) const noexcept
        {
            return view_.flush(conn, timeout_ms);
        }

        coroutine::AsyncCloseAwaiter close(Connection *conn) const noexcept
        {
            return view_.close(conn);
        }

        coroutine::AsyncReceiveFromAwaiter receive_from(Connection *conn, uint32_t timeout_ms = 0) const noexcept
        {
            return view_.receive_from(conn, timeout_ms);
        }

        timer::Timer *schedule(uint32_t delay_ms, std::function<void()> callback) const
        {
            return view_.schedule(delay_ms, std::move(callback));
        }

        timer::Timer *schedule_periodic(uint32_t delay_ms, uint32_t interval_ms,
                                        std::function<void()> callback, int repeat = 0) const
        {
            return view_.schedule_periodic(delay_ms, interval_ms, std::move(callback), repeat);
        }

        static void cancel_timer(timer::Timer *timer)
        {
            coroutine::RuntimeView::cancel_timer(timer);
        }

        void register_connection(const std::shared_ptr<Connection> &conn, std::shared_ptr<ConnectionHandler> handler) const
        {
            view_.register_connection(conn, std::move(handler));
        }
        void register_connection(Connection *conn, std::shared_ptr<ConnectionHandler> handler) const
        {
            view_.register_connection(conn, std::move(handler));
        }

        void update_channel(Channel *channel) const
        {
            view_.update_channel(channel);
        }

    private:
        coroutine::RuntimeView view_;
    };

    inline NetworkRuntime::RuntimeView NetworkRuntime::runtime_view() const noexcept
    {
        return RuntimeView(loop_, timer_manager_);
    }

} // namespace yuan::net

#endif
