#ifndef __BASE_UTILS_BASE128_H__
#define __BASE_UTILS_BASE128_H__
#include <cstdint>
#include <string>

namespace yuan::base::util
{
    std::string base128_encode(uint32_t number);

    uint32_t base128_decode(const std::string& data);
}

#endif