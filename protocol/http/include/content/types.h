#ifndef __NET_HTTP_CONTENT_TYPES_H__
#define __NET_HTTP_CONTENT_TYPES_H__
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "nlohmann/json.hpp"
#include "content_type.h"

namespace yuan::net::http 
{
    struct ContentData : std::enable_shared_from_this<ContentData>
    {
        virtual ~ContentData() {}

        template<typename T>
        std::shared_ptr<T> as()
        {
            static_assert(std::is_base_of<ContentData, T>::value, "should be inherited from ContentData");
            return std::dynamic_pointer_cast<T>(shared_from_this());
        }
    };

    struct TextContent : public ContentData
    {
        const char *begin   = nullptr;
        const char *end     = nullptr;

        std::string get_content() const
        {
            if (begin && end) {
                return std::string(begin, end);
            } else {
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

    struct FormDataItem : public std::enable_shared_from_this<FormDataItem>
    {
        FormDataType item_type_;
        virtual ~FormDataItem() {}
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
                std::cout << "removed tmp file: " << tmp_file_name_ << std::endl;
            }
        }
    };

    struct FormDataStreamItem : FormDataItem
    {
        std::string origin_name_;
        std::pair<std::string, std::unordered_map<std::string, std::string>> content_type_;
        const char *begin_ = nullptr;
        const char *end_ = nullptr;

        explicit FormDataStreamItem(const std::string &name, 
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
        std::unordered_map<std::string, std::shared_ptr<FormDataItem>> properties;

        template<typename T>
        std::shared_ptr<T> get_item(const std::string &name) 
        {
            static_assert(std::is_base_of<FormDataItem, T>::value, "should be inherited from FormDataItem");

            auto it = properties.find(name);
            if (it != properties.end()) {
                return std::dynamic_pointer_cast<T>(it->second);
            }

            return nullptr;
        }
    };

    struct RangeDataItem
    {
        struct Chunk
        {
            TextContent content;
            uint32_t from;
            uint32_t to;
            uint32_t length;
        } chunk;

        std::pair<std::string, std::unordered_map<std::string, std::string>> content_type_;
    };

    struct RangeDataContent : public ContentData
    {
        std::vector<std::shared_ptr<RangeDataItem>> contents;
    };

    struct Content
    {
        ContentType type_ = ContentType::not_support;
        std::shared_ptr<ContentData> content_data_ = nullptr;

        struct FileInfo
        {
            std::string tmp_file_name_;
            std::size_t file_size_ = 0;

            ~FileInfo()
            {
                if (!tmp_file_name_.empty()) {
                    std::remove(tmp_file_name_.c_str());
                    std::cout << "removed tmp file: " << tmp_file_name_ << std::endl;
                }
            }

        } file_info_;

        Content(ContentType type, ContentData *data) : type_(type), content_data_(data) {}

        template<typename T>
        std::shared_ptr<T> get_content_data()
        {
            static_assert(std::is_base_of<ContentData, T>::value, "should be inherited from ContentData");
            return std::dynamic_pointer_cast<T>(content_data_);
        }
    };
}

#endif