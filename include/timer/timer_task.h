#ifndef __TIMER_TASK_H__
#define __TIMER_TASK_H__

namespace timer 
{
    class Timer;

    class TimerTask
    {
    public:
        virtual void on_timer(Timer *timer) = 0;

        virtual void on_finished(Timer *timer) = 0;
        
    };
}

#endif
