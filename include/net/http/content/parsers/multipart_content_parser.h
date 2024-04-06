#ifndef __NET_HTTP_FORM_DATA_PARSER_H__
#define __NET_HTTP_FORM_DATA_PARSER_H__
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>
#include "net/http/content/content_parser.h"

namespace net::http 
{
    typedef std::pair<uint32_t, std::pair<std::string, std::unordered_map<std::string, std::string>>> ContentDisposition;

    enum class ContentDispositionType
    {
        unknow_ = -1,
        inline_ = 0,
        attachment_,
        form_data_,
    };

    ContentDispositionType get_content_disposition_type(const std::string &name);

    std::string content_disposition_to_string(ContentDispositionType type);

    class MultipartFormDataParser : public ContentParser
    {
    public:
        // 检查是否可以解析
        virtual bool can_parse(ContentType contentType);

        // 解析
        virtual bool parse(HttpPacket *packet);

    private:
        ContentDisposition parse_content_disposition(const char *begin, const char *end);
        std::pair<uint32_t, std::string> parse_part_value(const char *begin, const char *end);
        std::tuple<std::string, std::unordered_map<std::string, std::string>, uint32_t> parse_part_file_content(HttpPacket *packet, const char *begin, const char *end, const std::string &originName);
    };

    class MultipartByterangesParser : public ContentParser
    {
    public:
        // 检查是否可以解析
        virtual bool can_parse(ContentType contentType);

        // 解析
        virtual bool parse(HttpPacket *packet);
    
    private:
        std::vector<std::pair<uint32_t, uint32_t>> parse_range(const std::string &range);
        std::tuple<bool, uint32_t, uint32_t, uint32_t, uint32_t> parse_content_range(const char *begin, const char *end);
    };
}

#endif