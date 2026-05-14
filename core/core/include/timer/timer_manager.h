#ifndef __TIMER_MANAGER_H__
#define __TIMER_MANAGER_H__

#include <cstdint>
#include <algorithm>
#include <functional>
#include <memory>

#include "timer_handle.h"
#include "timer_task.h"

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

        virtual void tick() = 0;

        void run_due_timers()
        {
            tick();
        }

        virtual uint32_t get_time_unit() const = 0;

        virtual const char *backend_name() const
        {
            return "timer";
        }

        TimerHandle after(uint32_t milliseconds, std::function<void()> callback);

        TimerHandle after(uint32_t milliseconds, TimerTask *task)
        {
            Timer *timer = timeout(milliseconds, task);
            return timer ? TimerHandle(timer->handle_state()) : TimerHandle{};
        }

        TimerHandle every(uint32_t interval, std::function<void()> callback)
        {
            return every(interval, interval, std::move(callback));
        }

        TimerHandle every(uint32_t timeout, uint32_t interval, std::function<void()> callback, int32_t period = -1);

        TimerHandle every(uint32_t timeout, uint32_t interval, TimerTask *task, int32_t period = -1)
        {
            Timer *timer = this->interval(timeout, interval, task, period);
            return timer ? TimerHandle(timer->handle_state()) : TimerHandle{};
        }

        virtual uint32_t get_poll_timeout_ms(uint32_t idle_timeout_ms, uint32_t active_timeout_cap_ms)
        {
            const auto active_timeout = std::max<uint32_t>(1, get_time_unit());
            return std::min(active_timeout, active_timeout_cap_ms == 0 ? active_timeout : active_timeout_cap_ms);
        }

        uint32_t poll_timeout(uint32_t idle_timeout_ms, uint32_t active_timeout_cap_ms)
        {
            return get_poll_timeout_ms(idle_timeout_ms, active_timeout_cap_ms);
        }

    protected:
        virtual Timer *timeout(uint32_t milliseconds, TimerTask *task) = 0;

        virtual Timer *interval(uint32_t timeout, uint32_t interval, TimerTask *task, int32_t period = 0) = 0;

        virtual bool schedule(Timer *timer) = 0;
    };
}

#endif
