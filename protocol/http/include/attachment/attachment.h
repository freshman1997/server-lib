#ifndef __NET_HTTP_ATTACHMENT_H__
#define __NET_HTTP_ATTACHMENT_H__
#include <string>

namespace yuan::net::http
{
    struct AttachmentInfo
    {
        std::string tmp_file_name_;
        std::string origin_file_name_;
        std::string content_type_;
        std::size_t offset_ = 0;
        std::size_t length_ = 0;

        AttachmentInfo() = default;
        AttachmentInfo(const std::string &tmpFileName, const std::string &originFileName, const std::string &contentType, std::size_t offset, std::size_t length)
            : tmp_file_name_(tmpFileName), origin_file_name_(originFileName), content_type_(contentType), offset_(offset), length_(length) {}
        
        AttachmentInfo(const AttachmentInfo &other)
            : tmp_file_name_(other.tmp_file_name_), origin_file_name_(other.origin_file_name_), content_type_(other.content_type_), offset_(other.offset_), length_(other.length_) {}
        
        AttachmentInfo(AttachmentInfo &&other) noexcept
            : tmp_file_name_(std::move(other.tmp_file_name_)), origin_file_name_(std::move(other.origin_file_name_)), content_type_(std::move(other.content_type_)), offset_(other.offset_), length_(other.length_)
        {
            other.offset_ = 0;
            other.length_ = 0;
        }

        AttachmentInfo &operator=(const AttachmentInfo &other)
        {
            if (this != &other) {
                tmp_file_name_ = other.tmp_file_name_;
                origin_file_name_ = other.origin_file_name_;
                content_type_ = other.content_type_;
                offset_ = other.offset_;
                length_ = other.length_;
            }
            return *this;
        }

        AttachmentInfo &operator=(AttachmentInfo &&other) noexcept
        {
            if (this != &other) {
                tmp_file_name_ = std::move(other.tmp_file_name_);
                origin_file_name_ = std::move(other.origin_file_name_);
                content_type_ = std::move(other.content_type_);
                offset_ = other.offset_;
                length_ = other.length_;
            }
            return *this;
        }
    };
}

#endif // __NET_HTTP_ATTACHMENT_H__