#ifndef __TIMER_MANAGER_H__
#define __TIMER_MANAGER_H__

#include <cstdint>
#include <algorithm>

#include "timer_handle.h"

namespace yuan::timer
{
    class Timer;
    class TimerTask;

    class TimerManager
    {
    public:
        virtual ~TimerManager()
        {
        }

        virtual TimerHandle timeout_handle(uint32_t milliseconds, TimerTask *task)
        {
            Timer *timer = timeout(milliseconds, task);
            return timer ? TimerHandle(timer->handle_state()) : TimerHandle{};
        }

        virtual TimerHandle interval_handle(uint32_t timeout, uint32_t interval, TimerTask *task, int32_t period = 0)
        {
            Timer *timer = this->interval(timeout, interval, task, period);
            return timer ? TimerHandle(timer->handle_state()) : TimerHandle{};
        }

        virtual void tick() = 0;

        virtual uint32_t get_time_unit() const = 0;

        virtual uint32_t get_poll_timeout_ms(uint32_t idle_timeout_ms, uint32_t active_timeout_cap_ms)
        {
            const auto active_timeout = std::max<uint32_t>(1, get_time_unit());
            return std::min(active_timeout, active_timeout_cap_ms == 0 ? active_timeout : active_timeout_cap_ms);
        }

    protected:
        virtual Timer *timeout(uint32_t milliseconds, TimerTask *task) = 0;

        virtual Timer *interval(uint32_t timeout, uint32_t interval, TimerTask *task, int32_t period = 0) = 0;

        virtual bool schedule(Timer *timer) = 0;
    };
}

#endif
