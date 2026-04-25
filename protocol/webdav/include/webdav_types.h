#ifndef __NET_WEBDAV_TYPES_H__
#define __NET_WEBDAV_TYPES_H__

#include <cstdint>
#include <string>
#include <vector>

namespace yuan::net::webdav
{
    enum class Depth
    {
        zero,
        one,
        infinity
    };

    enum class Overwrite
    {
        yes,
        no
    };

    enum class LockScope
    {
        exclusive,
        shared
    };

    struct Property
    {
        std::string ns = "DAV:";
        std::string name;
        std::string value;
    };

    struct PropertyResult
    {
        std::string href;
        int status = 200;
        std::vector<Property> properties;
        std::vector<Property> missing_properties;
    };

    struct Quota
    {
        std::uint64_t used_bytes = 0;
        std::uint64_t available_bytes = 0;
    };
}

#endif
