#ifndef __EVENT_LOOH_H__
#define __EVENT_LOOH_H__
#include <atomic>
#include <coroutine>
#include <functional>
#include <memory>

#include "net/handler/event_handler.h"
#include "net/handler/connection_handler.h"

namespace yuan::timer
{
    class TimerManager;
}

namespace yuan::net 
{
    struct PollEvent;
    class Poller;
    class Socket;
    class Connection;
    class Channel;
    class SelectHandler;

    enum class EventLoopExitReason
    {
        quit_requested,
        coroutine_resume_requested,
    };

    class EventLoop : public EventHandler
    {
    public:
        class ExternalFdRegistration;

        // Threading model:
        // - loop() must be called from exactly one thread (the loop thread).
        // - While loop() is running, channels_/connections_ and the poller are
        //   owned by the loop thread. update_channel()/close_channel() marshal
        //   foreign-thread calls synchronously to preserve raw Channel
        //   lifetimes; before startup they execute directly.
        // - on_new_connection() may be called from any thread: off-loop calls
        //   are deferred to the loop thread; the queued functor keeps the
        //   connection alive until registration runs.
        // - queue_in_loop()/post_coroutine()/quit()/request_coroutine_resume()
        //   are safe from any thread and wake the loop as needed.
        // - No method may race with ~EventLoop(): the loop thread must have
        //   left loop() and callers must synchronize before destruction.
        EventLoop(Poller *_poller, timer::TimerManager *timer_manager);
        ~EventLoop();

    public:
        EventLoopExitReason loop();

        void on_new_connection(const std::shared_ptr<Connection> &conn) override;

        virtual void close_channel(Channel *channel) override;

        virtual void update_channel(Channel *channel) override;

        virtual void quit() override;

        void request_coroutine_resume();

        void queue_in_loop(std::function<void()> cb) override;

        void post_coroutine(std::coroutine_handle<> handle) noexcept override;

        // Execute an operation on the loop thread. Calls made before the
        // loop starts run synchronously; calls made while it is running are
        // queued and waited for. This is the boundary for operations that
        // touch loop-owned objects but cannot be made asynchronous safely.
        bool run_in_loop_sync(std::function<void()> operation);

        bool is_in_loop_thread() const noexcept override;

        bool is_running() const noexcept;

        bool wait_until_stopped(uint32_t timeout_ms = 0) const;

        bool accepts_poll_event_for_test(const PollEvent &event) const;

        std::unique_ptr<ExternalFdRegistration> register_external_fd(
            int fd,
            std::shared_ptr<SelectHandler> handler,
            int events);

    public:
        void wakeup();

    private:
        class HelperData;
        std::unique_ptr<HelperData> data_;

    };

    class EventLoop::ExternalFdRegistration
    {
    public:
        ExternalFdRegistration(EventLoop *loop, int fd, std::shared_ptr<SelectHandler> handler, int events);
        ~ExternalFdRegistration();

        ExternalFdRegistration(const ExternalFdRegistration &) = delete;
        ExternalFdRegistration &operator=(const ExternalFdRegistration &) = delete;
        ExternalFdRegistration(ExternalFdRegistration &&) = delete;
        ExternalFdRegistration &operator=(ExternalFdRegistration &&) = delete;

        // Thread-safe: from the loop thread (or before the loop starts) the
        // deregistration runs synchronously; from another thread it is
        // deferred to the loop thread and shared state keeps the channel
        // alive until then.
        void close();
        bool active() const noexcept;
        Channel *channel() noexcept;
        uint64_t generation() const noexcept;

    private:
        struct State;
        void close_state();

        EventLoop *loop_ = nullptr;
        std::shared_ptr<State> state_;
        std::atomic<bool> active_{false};
    };
}
#endif
