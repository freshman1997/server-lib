#ifndef __YUAN_TIMER_TIMER_MANAGER_FACTORY_H__
#define __YUAN_TIMER_TIMER_MANAGER_FACTORY_H__

#include <memory>

namespace yuan::timer
{
    class TimerManager;

    enum class TimerBackend
    {
        wheel,
        heap,
        automatic,
    };

    std::unique_ptr<TimerManager> create_timer_manager(TimerBackend backend = TimerBackend::automatic);
}

#endif
