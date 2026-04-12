#ifndef __YUAN_COROUTINE_SCHEDULER_H__
#define __YUAN_COROUTINE_SCHEDULER_H__

#include <coroutine>

namespace yuan::net
{
    class EventLoop;
}

namespace yuan::coroutine
{

class Scheduler
{
public:
    virtual ~Scheduler() = default;

    virtual void post(std::coroutine_handle<> handle) noexcept = 0;
};

class EventLoopScheduler final : public Scheduler
{
public:
    EventLoopScheduler() = default;
    explicit EventLoopScheduler(net::EventLoop *loop) noexcept
        : loop_(loop)
    {
    }

    net::EventLoop *event_loop() const noexcept
    {
        return loop_;
    }

    void post(std::coroutine_handle<> handle) noexcept override;

private:
    net::EventLoop *loop_ = nullptr;
};

class ScheduleAwaiter
{
public:
    explicit ScheduleAwaiter(Scheduler *scheduler) noexcept
        : scheduler_(scheduler)
    {
    }

    bool await_ready() const noexcept
    {
        return scheduler_ == nullptr;
    }

    bool await_suspend(std::coroutine_handle<> handle) const noexcept
    {
        if (!scheduler_) {
            return false;
        }

        scheduler_->post(handle);
        return true;
    }

    void await_resume() const noexcept
    {
    }

private:
    Scheduler *scheduler_ = nullptr;
};

inline ScheduleAwaiter schedule_on(Scheduler *scheduler) noexcept
{
    return ScheduleAwaiter(scheduler);
}

} // namespace yuan::coroutine

#endif
