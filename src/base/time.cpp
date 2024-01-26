#include <chrono>

#include "base/time.h"

namespace base::time
{
    uint32_t time_unit_ = 100;
    uint32_t tick_ = 0;

    int32_t get_tick_count()
    {
        auto time_now = std::chrono::system_clock::now();
        auto duration_in_ms = std::chrono::duration_cast<std::chrono::milliseconds>(time_now.time_since_epoch());
        return duration_in_ms.count();
    }

    void init_time(uint32_t unit)
    {
        time_unit_ = unit;
        tick_ = get_tick_count();
    }

    uint32_t get_passed_time()
    {
        if (tick_ == 0) {
            tick_ = get_tick_count();
            return 0;
        }

        uint32_t cur_tick = get_tick_count();
        uint32_t passed = (cur_tick - tick_) / time_unit_;
        tick_ += passed * time_unit_;

        return passed;
    }
}
