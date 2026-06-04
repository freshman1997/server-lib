#ifndef __EVENT_LOOH_H__
#define __EVENT_LOOH_H__
#include <coroutine>
#include <functional>
#include <memory>
#include <unordered_map>

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

    enum class EventLoopExitReason
    {
        quit_requested,
        coroutine_resume_requested,
    };

    class EventLoop : public EventHandler
    {
    public:
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

        void post_coroutine(std::coroutine_handle<> handle) noexcept;

        bool is_in_loop_thread() const noexcept;

        bool accepts_poll_event_for_test(const PollEvent &event) const;

    public:
        void wakeup();

    private:
        class HelperData;
        std::unique_ptr<HelperData> data_;
    };
}
#endif
