#ifndef __BASE_TIME_H__
#define __BASE_TIME_H__
#include <cstdint>

namespace base::time
{
    uint32_t get_tick_count();

    void init_time(uint32_t unit);

    uint32_t get_passed_time();
}

#endif