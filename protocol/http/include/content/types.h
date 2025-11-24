#ifndef __NET_HTTP_CONTENT_TYPES_H__
#define __NET_HTTP_CONTENT_TYPES_H__
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "buffer/buffer_reader.h"
#include "content_type.h"
#include "nlohmann/json.hpp"

namespace yuan::net::http 
{
    struct ContentData
    {
        virtual ~ContentData() = default;
    };

    struct TextContent : ContentData
    {
        size_t begin   = 0;
        size_t len     = 0;

        std::string get_content(buffer::BufferReader &reader) const
        {
            if (begin && len > 0) {
                std::string content;
                content.reserve(len);
                if (const int r = reader.read(content.data(), len); r >= 0) {
                    return content;
                }
            }
            return {};
        }
    };

    struct JsonContent : ContentData
    {
        nlohmann::json jval;
    };

    enum class FormDataType
    {
        none_,
        string_,
        stream_,
        file_,
    };

    struct FormDataItem
    {
        FormDataType item_type_ = FormDataType::none_;
        virtual ~FormDataItem() = default;
    };

    struct FormDataStringItem : FormDataItem
    {
        std::string value_;
        explicit FormDataStringItem(std::string val) : value_(std::move(val)) {
            item_type_ = FormDataType::string_;
        }
    };

    struct FormDataFileItem : FormDataItem
    {
        std::string origin_name_;
        std::string tmp_file_name_;
        std::unordered_map<std::string, std::string> content_type_;
        FormDataFileItem(std::string origin, std::string tmpName, const std::unordered_map<std::string, std::string>&ctype)
            : origin_name_(std::move(origin)), tmp_file_name_(std::move(tmpName)),content_type_(ctype)  {
            item_type_ = FormDataType::file_;
            content_type_.erase("____tmp_file_name");
        }

        ~FormDataFileItem() override {
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
        size_t begin_ = 0;
        size_t len_ = 0;

        explicit FormDataStreamItem(std::string name,
            const std::pair<std::string, std::unordered_map<std::string, std::string>> &type,
                                    const size_t begin, const size_t len)
            : origin_name_(std::move(name)), content_type_(type), begin_(begin), len_(len) {

            item_type_ = FormDataType::stream_;
        }

        std::size_t get_content_length() const
        {
            return len_;
        }
    };

    struct FormDataContent : public ContentData
    {
        std::string type;
        std::unordered_map<std::string, std::shared_ptr<FormDataItem>> properties;
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
        std::vector<RangeDataItem *> contents;
        ~RangeDataContent() override {
            for (const auto it : contents) {
                delete it;
            }
            contents.clear();
        }
    };

    struct Content
    {
        ContentType type_ = ContentType::not_support;
        ContentData *content_data_ = nullptr;

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
        ~Content() 
        {
            delete content_data_;
        }
    };
}

#endif