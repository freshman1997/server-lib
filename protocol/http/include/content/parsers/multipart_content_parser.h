#ifndef __NET_HTTP_FORM_DATA_PARSER_H__
#define __NET_HTTP_FORM_DATA_PARSER_H__
#include "buffer/buffer_reader.h"

#include "content/content_parser.h"
#include <cstdint>
#include <unordered_map>
#include <utility>

namespace yuan::net::http 
{
    typedef std::pair<uint32_t, std::pair<std::string, std::unordered_map<std::string, std::string>>> ContentDisposition;

    class MultipartFormDataParser final : public ContentParser
    {
    public:
        // 检查是否可以解析
        bool can_parse(ContentType contentType) override;

        // 解析
        bool parse(HttpPacket *packet) override;

    private:
        struct StreamResult
        {
            std::string type_;
            std::unordered_map<std::string, std::string> extra_;
            size_t stream_begin_;
            size_t len_;
        };

        static ContentDisposition parse_content_disposition(buffer::BufferReader &reader);
        static std::pair<int, std::string> parse_part_value(buffer::BufferReader &reader, const std::string &boundary);
        static int parse_part_file_content(StreamResult &result, const HttpPacket *packet, buffer::BufferReader &reader, const std::string &originName, const std::string &boundary);
    };

    class MultipartByterangesParser : public ContentParser
    {
    public:
        // 检查是否可以解析
        bool can_parse(ContentType contentType) override;

        // 解析
        bool parse(HttpPacket *packet) override;
    
    private:
        static std::tuple<bool, uint32_t, uint32_t, uint32_t, uint32_t> parse_content_range(buffer::BufferReader &reader);
    };
}

#endif