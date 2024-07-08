#ifndef __NET_HTTP_FORM_DATA_PARSER_H__
#define __NET_HTTP_FORM_DATA_PARSER_H__
#include <cstdint>
#include <unordered_map>
#include <utility>
#include "net/http/content/content_parser.h"

namespace net::http 
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
        ContentDisposition parse_content_disposition(const char *begin, const char *end);
        std::pair<uint32_t, std::string> parse_part_value(const char *begin, const char *end);
        std::tuple<std::string, std::unordered_map<std::string, std::string>, uint32_t> parse_part_file_content(HttpPacket *packet, const char *begin, const char *end, const std::string &originName);
    };

    class MultipartByterangesParser : public ContentParser
    {
    public:
        // 检查是否可以解析
        bool can_parse(ContentType contentType) override;

        // 解析
        bool parse(HttpPacket *packet) override;
    
    private:
        std::tuple<bool, uint32_t, uint32_t, uint32_t, uint32_t> parse_content_range(const char *begin, const char *end);
    };
}

#endif