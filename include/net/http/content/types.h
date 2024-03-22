#ifndef __NET_HTTP_CONTENT_TYPES_H__
#define __NET_HTTP_CONTENT_TYPES_H__
#include <string>
#include <unordered_map>
#include "net/http/content_type.h"
#include "nlohmann/json.hpp"

namespace net::http 
{
    struct TextContent
    {
        const char *begin   = nullptr;
        const char *end     = nullptr;

        std::string get_content() 
        {
            if (begin && end) {
                return std::string(begin, end);
            }
            return {};
        }
    };

    struct JsonContent
    {
        nlohmann::json jval;
    };

    struct FormDataContent
    {
        std::string type;
        std::unordered_map<std::string, std::string> properties;
    };

    struct Content
    {
        content_type type = content_type::not_support;
        void *content_data_ = nullptr;
    };
}

#endif