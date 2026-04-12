#include "base/time.h"

#include <cassert>
#include <iostream>

int main()
{
    using namespace yuan::base::time;

    reset_test_time();

    const auto real_steady = steady_now_ms();
    const auto real_system = system_now_ms();
    assert(real_steady > 0);
    assert(real_system > 0);

    set_steady_time_for_test(123456);
    set_system_time_for_test(987654);
    assert(steady_now_ms() == 123456);
    assert(steady_now_us() == 123456000ULL);
    assert(system_now_ms() == 987654);
    assert(system_now_us() == 987654000ULL);
    assert(system_now_seconds() == 987);

    advance_steady_time_for_test(44);
    advance_system_time_for_test(346);
    assert(steady_now_ms() == 123500);
    assert(system_now_ms() == 988000);
    assert(now() == static_cast<uint32_t>(123500));

    reset_test_time();
    assert(steady_now_ms() != 123500);
    assert(system_now_ms() != 988000);

    std::cout << "time api test passed" << std::endl;
    return 0;
}
