#ifndef __NET_HTTP_CONTENT_TYPES_H__
#define __NET_HTTP_CONTENT_TYPES_H__
#include <iostream>
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

    enum class FormDataType
    {
        string_,
        stream_,
        file_,
    };

    struct FormDataItem
    {
        FormDataType item_type_;
    };

    struct FormDataStringItem : FormDataItem
    {
        std::string value_;
        FormDataStringItem(const std::string &val) : value_(std::move(val)) {
            item_type_ = FormDataType::string_;
        }
    };

    struct FormDataFileItem : FormDataItem
    {
        std::string origin_name_;
        std::string tmp_file_name_;
        std::unordered_map<std::string, std::string> content_type_;
        FormDataFileItem(const std::string &origin, const std::string &tmpName, const std::unordered_map<std::string, std::string>&ctype) 
            : origin_name_(std::move(origin)), tmp_file_name_(std::move(tmpName)),content_type_(std::move(ctype))  {
            item_type_ = FormDataType::file_;
        }

        ~FormDataFileItem() {
            if (!tmp_file_name_.empty()) {
                std::remove(tmp_file_name_.c_str());
                std::cout << "removed tmp file " << tmp_file_name_ << std::endl;
            }
        }
    };

    struct FormDataStreamItem : FormDataItem
    {
        std::string origin_name_;
        std::pair<std::string, std::unordered_map<std::string, std::string>> content_type_;
        const char *begin_ = nullptr;
        const char *end_ = nullptr;

        FormDataStreamItem(const std::string &name, 
            const std::pair<std::string, std::unordered_map<std::string, std::string>> &type, 
            const char *begin, const char *end) 
            : origin_name_(std::move(name)), content_type_(std::move(type)), begin_(begin), end_(end) {

            item_type_ = FormDataType::stream_;
        }

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