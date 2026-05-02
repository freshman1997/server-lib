#ifndef __WHEEL_TIMER_MANAGER_H__
#define __WHEEL_TIMER_MANAGER_H__
#include <cstdint>
#include <memory>
#include <vector>

#include "timer_manager.h"

namespace yuan::timer
{
    class Wheel;
    class WheelTimer;
    class WheelTimerItem;

    class WheelTimerManager : public TimerManager
    {
    public:
        WheelTimerManager();
        ~WheelTimerManager();

    public:
        virtual Timer *timeout(uint32_t milliseconds, TimerTask *task);
        virtual Timer *interval(uint32_t timeout, uint32_t interval, TimerTask *task, int32_t period = 0);
        virtual bool schedule(Timer *timer);
        virtual void tick();
        virtual uint32_t get_time_unit() const override;

    private:
        void place_timer(WheelTimer *timer);

    private:
        Timer *schedule(uint32_t timeout, uint32_t interval, TimerTask *task, int32_t period);

    private:
        uint32_t time_unit_;
        uint32_t count_;
        std::unique_ptr<WheelTimerItem> helper_item_;
        std::vector<std::unique_ptr<Wheel> > wheels_;
        std::vector<std::unique_ptr<WheelTimer> > timers_;
    };
}

#endif
