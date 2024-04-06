#ifndef __TIMER_MANAGER_H__
#define __TIMER_MANAGER_H__

#include <cstdint>

namespace timer
{
    class Timer;
    class TimerTask;

    class TimerManager
    {
    public:
        virtual ~TimerManager() {}

        virtual Timer * timeout(uint32_t milliseconds, TimerTask *task) = 0;

        virtual Timer * interval(uint32_t timeout, uint32_t interval, TimerTask *task, int32_t period = 0) = 0;

        virtual bool schedule(Timer *timer) = 0;

        virtual void tick() = 0;

        virtual uint32_t get_time_unit() = 0;
    };
}

#endif
