#ifndef __YUAN_COROUTINE_EVENT_LOOP_TIMEOUT_AWAITABLE_H__
#define __YUAN_COROUTINE_EVENT_LOOP_TIMEOUT_AWAITABLE_H__

#include <coroutine>
#include <cstdint>
#include <memory>

#include "event/event_loop.h"
#include "timer/timer.h"
#include "timer/timer_manager.h"
#include "timer/timer_util.hpp"

namespace yuan::coroutine
{

class ScheduledTimeoutAwaiter
{
public:
    ScheduledTimeoutAwaiter(net::EventLoop *loop, timer::TimerManager *timer_manager, uint32_t timeout_ms) noexcept
        : loop_(loop)
        , timer_manager_(timer_manager)
        , timeout_ms_(timeout_ms)
    {
    }

    bool await_ready() const noexcept
    {
        return loop_ == nullptr;
    }

    bool await_suspend(std::coroutine_handle<> handle)
    {
        if (!loop_) {
            return false;
        }

        state_ = std::make_shared<State>();
        state_->loop = loop_;
        state_->handle = handle;

        if (timeout_ms_ == 0 || !timer_manager_) {
            loop_->post_coroutine(handle);
            return true;
        }

        state_->timer = timer::TimerUtil::build_timeout_handle(
            timer_manager_,
            timeout_ms_,
            [state = state_]() {
                state->timed_out = true;
                state->timer.reset();
                if (state->loop && state->handle) {
                    state->loop->post_coroutine(state->handle);
                }
            });

        return true;
    }

    bool await_resume() const noexcept
    {
        return state_ && state_->timed_out;
    }

private:
    struct State
    {
        net::EventLoop *loop = nullptr;
        timer::TimerHandle timer;
        std::coroutine_handle<> handle{};
        bool timed_out = false;
    };

    net::EventLoop *loop_ = nullptr;
    timer::TimerManager *timer_manager_ = nullptr;
    uint32_t timeout_ms_ = 0;
    std::shared_ptr<State> state_{};
};

inline ScheduledTimeoutAwaiter sleep_in_event_loop(
    net::EventLoop *loop,
    timer::TimerManager *timer_manager,
    uint32_t timeout_ms) noexcept
{
    return ScheduledTimeoutAwaiter(loop, timer_manager, timeout_ms);
}

} // namespace yuan::coroutine

#endif
