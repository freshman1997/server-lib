#ifndef __YUAN_TIMER_TIMER_HANDLE_H__
#define __YUAN_TIMER_TIMER_HANDLE_H__

#include "timer.h"

namespace yuan::timer
{
    class TimerHandle
    {
    public:
        TimerHandle() = default;
        explicit TimerHandle(Timer *timer) noexcept
            : timer_(timer)
        {
        }

        Timer *get() const noexcept
        {
            return timer_;
        }

        explicit operator bool() const noexcept
        {
            return timer_ != nullptr;
        }

        void reset(Timer *timer = nullptr) noexcept
        {
            timer_ = timer;
        }

        void cancel() const
        {
            if (timer_) {
                timer_->cancel();
            }
        }

    private:
        Timer *timer_ = nullptr;
    };
}

#endif
