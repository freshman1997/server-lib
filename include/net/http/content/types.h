#ifndef __NET_HTTP_CONTENT_TYPES_H__
#define __NET_HTTP_CONTENT_TYPES_H__
#include <string>
#include <unordered_map>
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

    struct FormDataItem
    {
    };

    struct FormDataStringItem : FormDataItem
    {
        std::string value_;
        FormDataStringItem(const std::string &val) : value_(std::move(val)) {}
    };

    struct FormDataStreamItem : FormDataItem
    {
        std::string originName_;
        std::pair<std::string, std::unordered_map<std::string, std::string>> content_type_;
        const char *begin_ = nullptr;
        const char *end_ = nullptr;

        FormDataStreamItem(const std::string &name, 
            const std::pair<std::string, std::unordered_map<std::string, std::string>> &type, 
            const char *begin, const char *end) 
            : originName_(std::move(name)), content_type_(std::move(type)), begin_(begin), end_(end) {}

        std::size_t get_content_length()
        {
            if (begin_ && end_) {
                return end_ - begin_;
            }

            return 0;
        }
    };

    struct FormDataContent : public ContentData
    {
        std::string type;
        std::unordered_map<std::string, FormDataItem> properties;
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