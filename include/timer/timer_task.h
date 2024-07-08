#ifndef __TIMER_TASK_H__
#define __TIMER_TASK_H__

namespace timer 
{
    class Timer;

    class TimerTask
    {
    public:
        virtual ~TimerTask() {}
        
        virtual void on_timer(Timer *timer) = 0;
    };
}

#endif
