#ifndef __TIMER_H__
#define __TIMER_H__

namespace timer
{

    enum class TimerState : char
    {
        init = 0,
        processing,
        done,
        cancal,
    };

    class Timer
    {
    public:
        virtual ~Timer() {}

        virtual bool ready() = 0;

        virtual void cancel() = 0;

        virtual void reset() = 0;

        virtual bool is_processing() = 0;

        virtual bool is_done() = 0;

        virtual bool is_cancel() = 0;
    
    };
}

#endif