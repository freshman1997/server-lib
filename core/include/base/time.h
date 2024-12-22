#ifndef __BASE_TIME_H__
#define __BASE_TIME_H__
#include <cstdint>

namespace yuan::base::time
{
    uint64_t get_tick_count();

    void init_time(uint32_t unit);

    uint64_t get_passed_time();

    uint32_t now();
}

#endif