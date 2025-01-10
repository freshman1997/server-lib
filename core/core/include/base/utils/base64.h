#ifndef __BASE_UTILS_BASE64_H__
#define __BASE_UTILS_BASE64_H__
#include <string>

namespace yuan::base::util
{
    std::string base64_encode(const std::string& data);

    std::string base64_decode(const std::string& data);
}

#endif