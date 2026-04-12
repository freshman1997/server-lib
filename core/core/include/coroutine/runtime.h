#ifndef __YUAN_COROUTINE_RUNTIME_H__
#define __YUAN_COROUTINE_RUNTIME_H__

#include <cstdint>
#include <functional>

#include "coroutine/event_loop_timeout_awaitable.h"
#include "coroutine/queue_in_loop_awaitable.h"
#include "coroutine/scheduler.h"

namespace yuan::timer
{
    class TimerManager;
}

namespace yuan::coroutine
{

class RuntimeView
{
public:
    RuntimeView() = default;

    RuntimeView(net::EventLoop *event_loop, timer::TimerManager *timer_manager) noexcept
        : event_loop_(event_loop)
        , timer_manager_(timer_manager)
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

private:
    net::EventLoop *event_loop_ = nullptr;
    timer::TimerManager *timer_manager_ = nullptr;
    mutable EventLoopScheduler scheduler_{};
};

} // namespace yuan::coroutine

#endif
