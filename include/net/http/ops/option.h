#ifndef __HTTP_SERVER_OPTION_H__
#define __HTTP_SERVER_OPTION_H__
#include <cstdint>

namespace net::http 
{
    // 最大包体长度 20 m
    static const uint32_t client_max_content_length = 1024 * 1024 * 20;
}

#endif