#ifndef __EVENT_LOOH_H__
#define __EVENT_LOOH_H__
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

        bool is_in_loop_thread() const noexcept override;

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

        void close();
        bool active() const noexcept;
        Channel *channel() noexcept;
        uint64_t generation() const noexcept;

    private:
        EventLoop *loop_ = nullptr;
        std::shared_ptr<SelectHandler> handler_;
        std::unique_ptr<Channel> channel_;
        bool active_ = false;
    };
}
#endif
