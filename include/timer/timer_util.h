#ifndef __TIMER_TIMER_UTIL_H__
#define __TIMER_TIMER_UTIL_H__
#include <cstdint>

namespace timer 
{
    class TimerManager;
    class Timer;

    class TimerUtil
    {
    public:
        template<typename T>
        static Timer * build_period_timer(TimerManager *manager, uint32_t timeout, uint32_t interval, T *object, void (T::*func)(Timer *));

        template<typename T>
        static Timer * build_timeout_timer(TimerManager *manager, uint32_t timeout, T *object, void (T::*func)(Timer *));
    };

}

#include "timer_util.hpp"

#endif