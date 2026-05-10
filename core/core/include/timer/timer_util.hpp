#ifndef __YUAN_TIMER_TIMER_UTIL_HPP__
#define __YUAN_TIMER_TIMER_UTIL_HPP__

#include "timer_task.h"
#include "timer.h"
#include "timer_manager.h"
#include <functional>
#include <utility>

namespace yuan::timer
{
    class TimerManager;
    class Timer;

    class TimerUtil
    {
    public:
        inline static TimerHandle build_period_handle(TimerManager *manager, uint32_t timeout, uint32_t interval, std::function<void()> func, int period = 0);

        inline static TimerHandle build_timeout_handle(TimerManager *manager, uint32_t timeout, std::function<void()> func);
    };

    class SimpleTimerTask : public TimerTask
    {
    public:
        explicit SimpleTimerTask(std::function<void()> func)
            : func_(std::move(func))
        {
        }

        virtual void on_timer(Timer *timer)
        {
            (void)timer;
            func_();
        }

        virtual void on_finished(Timer *timer)
        {
            (void)timer;
        }

        virtual bool need_free() const override
        {
            return true;
        }

    private:
        std::function<void()> func_;
    };

    inline TimerHandle TimerUtil::build_timeout_handle(TimerManager *manager, uint32_t timeout, std::function<void()> func)
    {
        if (!manager) {
            return {};
        }
        return manager->timeout_handle(timeout, new SimpleTimerTask(std::move(func)));
    }

    inline TimerHandle TimerUtil::build_period_handle(TimerManager *manager, uint32_t timeout, uint32_t interval, std::function<void()> func, int period)
    {
        if (!manager) {
            return {};
        }
        return manager->interval_handle(timeout, interval, new SimpleTimerTask(std::move(func)), period);
    }
}

#endif
