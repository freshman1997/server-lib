#ifndef __NET_HTTP_CONTENT_TYPES_H__
#define __NET_HTTP_CONTENT_TYPES_H__
#include <string>
#include <unordered_map>
#include <utility>
#include "net/http/content_type.h"
#include "nlohmann/json.hpp"

namespace net::http 
{
    struct ContentData
    {
        virtual ~ContentData() {}
    };

    struct TextContent : public ContentData
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

    struct JsonContent : public ContentData
    {
        nlohmann::json jval;
    };

    struct FormDataContent : public ContentData
    {
        std::string type;
        // <key, <isFile, value>>
        std::unordered_map<std::string, std::pair<bool, std::string>> properties;
        ~FormDataContent();
    };

    struct Content
    {
        content_type type_ = content_type::not_support;
        ContentData *content_data_ = nullptr;

        Content(content_type type, ContentData *data) : type_(type), content_data_(data) {}
        ~Content() 
        {
            if (content_data_) {
                delete content_data_;
            }
        }
    };
}

#endif