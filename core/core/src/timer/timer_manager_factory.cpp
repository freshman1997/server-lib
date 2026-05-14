#include "timer/timer_manager_factory.h"

#include "timer/heap_timer_manager.h"
#include "timer/wheel_timer_manager.h"

namespace yuan::timer
{
    std::unique_ptr<TimerManager> create_timer_manager(TimerBackend backend)
    {
        if (backend == TimerBackend::automatic) {
            backend = TimerBackend::wheel;
        }

        if (backend == TimerBackend::heap) {
            return std::make_unique<HeapTimerManager>();
        }

        return std::make_unique<WheelTimerManager>();
    }
}
