#ifndef __TIMER_MANAGER_H__
#define __TIMER_MANAGER_H__

namespace timer
{
    class Timer;

    class TimerManager
    {
    public:
        void cancel();
        void remove();
    };
}

#endif
