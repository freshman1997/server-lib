#ifndef __YUAN_COROUTINE_COMPLETION_EVENT_H__
#define __YUAN_COROUTINE_COMPLETION_EVENT_H__

#include <coroutine>
#include <cstdint>
#include <memory>

#include "event/event_loop.h"
#include "timer/timer.h"
#include "timer/timer_manager.h"
#include "timer/timer_util.hpp"

namespace yuan::coroutine
{

class CompletionEvent
{
public:
    struct State
    {
        net::EventLoop *loop = nullptr;
        std::coroutine_handle<> waiter{};
        timer::Timer *timer = nullptr;
        bool completed = false;
        bool timed_out = false;
        bool resumed = false;
    };

    CompletionEvent() = default;

    void reset(net::EventLoop *loop = nullptr) noexcept
    {
        state_ = std::make_shared<State>();
        state_->loop = loop;
    }

    void bind_loop(net::EventLoop *loop) noexcept
    {
        ensure_state();
        state_->loop = loop;
    }

    void notify() noexcept
    {
        ensure_state();

        state_->completed = true;
        if (state_->timer) {
            state_->timer->cancel();
            state_->timer = nullptr;
        }

        resume_waiter_if_needed();
    }

    bool completed() const noexcept
    {
        return state_ && state_->completed;
    }

    class Waiter
    {
    public:
        explicit Waiter(std::shared_ptr<State> state) noexcept
            : state_(std::move(state))
        {
        }

        bool await_ready() const noexcept
        {
            return !state_ || state_->completed;
        }

        bool await_suspend(std::coroutine_handle<> handle) noexcept
        {
            if (!state_) {
                return false;
            }

            state_->waiter = handle;
            return !state_->completed;
        }

        void await_resume() const noexcept
        {
        }

    private:
        std::shared_ptr<State> state_{};
    };

    class WaitForAwaiter
    {
    public:
        WaitForAwaiter(std::shared_ptr<State> state, timer::TimerManager *timer_manager, uint32_t timeout_ms) noexcept
            : state_(std::move(state))
            , timer_manager_(timer_manager)
            , timeout_ms_(timeout_ms)
        {
        }

        bool await_ready() const noexcept
        {
            return !state_ || state_->completed;
        }

        bool await_suspend(std::coroutine_handle<> handle)
        {
            if (!state_) {
                return false;
            }

            state_->waiter = handle;
            if (state_->completed) {
                return false;
            }

            if (timeout_ms_ > 0 && timer_manager_) {
                state_->timer = timer::TimerUtil::build_timeout_timer(
                    timer_manager_,
                    timeout_ms_,
                    [state = state_](timer::Timer *timer) {
                        if (!state || state->completed || state->resumed) {
                            if (timer) {
                                timer->cancel();
                            }
                            return;
                        }

                        state->timed_out = true;
                        state->timer = nullptr;
                        if (timer) {
                            timer->cancel();
                        }

                        if (state->loop && state->waiter) {
                            state->resumed = true;
                            state->loop->post_coroutine(state->waiter);
                        }
                    });
            }

            return true;
        }

        bool await_resume() const noexcept
        {
            return state_ && state_->timed_out;
        }

    private:
        std::shared_ptr<State> state_{};
        timer::TimerManager *timer_manager_ = nullptr;
        uint32_t timeout_ms_ = 0;
    };

    Waiter wait() noexcept
    {
        ensure_state();
        return Waiter(state_);
    }

    WaitForAwaiter wait_for(timer::TimerManager *timer_manager, uint32_t timeout_ms) noexcept
    {
        ensure_state();
        return WaitForAwaiter(state_, timer_manager, timeout_ms);
    }

private:
    void ensure_state() noexcept
    {
        if (!state_) {
            state_ = std::make_shared<State>();
        }
    }

    void resume_waiter_if_needed() noexcept
    {
        if (!state_ || state_->resumed || !state_->loop || !state_->waiter) {
            return;
        }

        state_->resumed = true;
        state_->loop->post_coroutine(state_->waiter);
    }

    std::shared_ptr<State> state_{};
};

} // namespace yuan::coroutine

#endif
