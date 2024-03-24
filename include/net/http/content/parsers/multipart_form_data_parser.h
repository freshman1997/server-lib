#ifndef __NET_HTTP_FORM_DATA_PARSER_H__
#define __NET_HTTP_FORM_DATA_PARSER_H__
#include <unordered_map>
#include "net/http/content/content_parser.h"

namespace net::http 
{
    typedef std::pair<uint32_t, std::pair<std::string, std::unordered_map<std::string, std::string>>> ContentDispistion;

    enum class ContentDispistionType
    {
        unknow_ = -1,
        inline_ = 0,
        attachment_,
        form_data_,
    };

    ContentDispistionType get_content_disposition_type(const std::string &name);

    std::string content_disposition_to_string(ContentDispistionType type);

    class MultipartFormDataParser : public ContentParser
    {
    public:
        // 检查是否可以解析
        virtual bool can_parse(const content_type contentType);

        // 解析
        virtual bool parse(HttpRequest *req);

    public:
        ContentDispistion parse_content_disposition(const char *begin, const char *end);
        std::pair<uint32_t, std::string> parse_part_value(const char *begin, const char *end);
        std::tuple<bool, uint32_t, std::string> parse_part_file_content(HttpRequest *req, const char *begin, const char *end, const std::string &originName);
    };
}

#endif