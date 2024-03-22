#ifndef __NET_HTTP_CONTENT_PARSER_H__
#define __NET_HTTP_CONTENT_PARSER_H__
#include <string>
#include <unordered_map>

#include "net/http/content_type.h"

namespace net::http 
{
    class HttpRequest;
    typedef std::pair<uint32_t, std::pair<std::string, std::unordered_map<std::string, std::string>>> ContentDispistion;

    class ContentParser
    {
    public:
        ~ContentParser() {}

    public:
        // 检查是否可以解析
        virtual bool can_parse(const std::string &contentType) = 0;
        virtual bool can_parse(const content_type contentType) = 0;

        // 解析
        virtual bool parse(HttpRequest *req) = 0;

    public:
        ContentDispistion parse_content_disposition(const char *begin, const char *end);
    };

}

#endif