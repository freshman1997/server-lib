#ifndef __TIMER_TASK_H__
#define __TIMER_TASK_H__

namespace yuan::timer 
{
    class Timer;

    class TimerTask
    {
    public:
        virtual ~TimerTask() {}
        
        virtual void on_timer(Timer *timer) = 0;

        virtual void on_finished(Timer *timer)
        {
            
        }

        virtual bool need_free()
        {
            return false;
        }
    };
}

#endif
