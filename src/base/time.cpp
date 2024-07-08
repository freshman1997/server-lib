#include <chrono>

#include "base/time.h"

namespace base::time
{
    static uint32_t time_unit_ = 100;
    static uint64_t tick_ = 0;

    uint64_t get_tick_count()
    {
        const auto time_now = std::chrono::system_clock::now();
        const auto duration_in_ms = std::chrono::duration_cast<std::chrono::milliseconds>(time_now.time_since_epoch());
        return duration_in_ms.count();
    }

    void init_time(const uint32_t unit)
    {
        time_unit_ = unit;
        tick_ = get_tick_count();
    }

    uint64_t get_passed_time()
    {
        if (tick_ == 0) {
            tick_ = get_tick_count();
            return 0;
        }

        const uint64_t cur_tick = get_tick_count();
        const uint64_t passed = (cur_tick - tick_) / time_unit_;
        tick_ += passed * time_unit_;

        return passed;
    }
}
