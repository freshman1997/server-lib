#ifndef __YUAN_TIMER_TIMER_HANDLE_H__
#define __YUAN_TIMER_TIMER_HANDLE_H__

#include "timer.h"

#include <utility>

namespace yuan::timer
{
    class TimerHandle
    {
    public:
        TimerHandle() = default;
        explicit TimerHandle(std::shared_ptr<TimerHandleState> state) noexcept
            : state_(std::move(state))
        {
        }

        explicit operator bool() const noexcept
        {
            return state_ && state_->active();
        }

        void reset(TimerHandle timer = {}) noexcept
        {
            state_ = std::move(timer.state_);
        }

        void cancel() const
        {
            if (state_) {
                state_->cancel();
            }
        }

    private:
        std::shared_ptr<TimerHandleState> state_;
    };
}

#endif
