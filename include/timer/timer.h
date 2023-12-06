#ifndef __TIMER_H__
#define __TIMER_H__

namespace timer
{
    class timer
    {
    public:
        virtual void timeout() = 0;
        virtual void interval(int inter, int time, int expired) = 0;
    };
}

#endif