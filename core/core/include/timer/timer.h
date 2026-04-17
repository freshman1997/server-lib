#ifndef __TIMER_H__
#define __TIMER_H__

namespace yuan::timer
{

    enum class TimerState : char {
        init = 0,
        processing,
        done,
        cancal,
    };

    class TimerTask;
    class Timer
    {
    public:
        virtual ~Timer()
        {
        }

        virtual bool ready() const = 0;

        virtual void cancel() = 0;

        virtual void reset() = 0;

        virtual bool is_processing() const = 0;

        virtual bool is_done() const = 0;

        virtual bool is_cancel() const = 0;

        virtual TimerTask *get_task() const = 0;
    };
}

#endif