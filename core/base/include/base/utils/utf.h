#ifndef __BASE_UTILS_UTF_H__
#define __BASE_UTILS_UTF_H__

#include <cstddef>
#include <cstdint>
#include <span>

namespace yuan::base::util
{
    bool is_valid_utf8(std::span<const std::uint8_t> data);

    inline bool is_valid_utf8(const char *data, std::size_t size)
    {
        return is_valid_utf8(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(data), size));
    }
}

#endif
