#include "base/time.h"
#include "timer/heap_timer_manager.h"
#include "timer/timer_manager.h"
#include "timer/timer_manager_factory.h"
#include "timer/wheel_timer_manager.h"

#include <cassert>
#include <iostream>
#include <string>

namespace
{
    void run_once(yuan::timer::TimerManager &manager)
    {
        int fired = 0;
        auto timer = manager.after(10, [&fired]() {
            ++fired;
        });

        assert(timer);
        yuan::base::time::advance_steady_time_for_test(10);
        manager.run_due_timers();
        assert(fired == 1);
        assert(!timer);
    }

    void run_periodic(yuan::timer::TimerManager &manager)
    {
        int fired = 0;
        auto timer = manager.every(5, 5, [&fired]() {
            ++fired;
        }, 3);

        assert(timer);
        for (int i = 0; i < 3; ++i) {
            yuan::base::time::advance_steady_time_for_test(5);
            manager.run_due_timers();
        }
        assert(fired == 3);
        assert(!timer);
    }
}

int main()
{
    yuan::base::time::set_steady_time_for_test(1000);
    {
        yuan::timer::HeapTimerManager manager;
        run_once(manager);
        run_periodic(manager);
    }

    yuan::base::time::set_steady_time_for_test(2000);
    {
        yuan::timer::WheelTimerManager manager;
        run_once(manager);
        run_periodic(manager);
    }

    auto heap = yuan::timer::create_timer_manager(yuan::timer::TimerBackend::heap);
    assert(heap);
    assert(std::string(heap->backend_name()) == "heap");

    auto wheel = yuan::timer::create_timer_manager(yuan::timer::TimerBackend::wheel);
    assert(wheel);
    assert(std::string(wheel->backend_name()) == "wheel");

    yuan::base::time::reset_test_time();
    std::cout << "timer backend test passed\n";
    return 0;
}
