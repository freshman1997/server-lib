#ifndef __NET_HTTP_CONTENT_PARSER_H__
#define __NET_HTTP_CONTENT_PARSER_H__
#include <string>

#include "content_type.h"

namespace yuan::net::http 
{
    class HttpPacket;

    class ContentParser
    {
    public:
        virtual ~ContentParser() {}

    public:
        // 检查是否可以解析
        bool can_parse(const std::string &contentType);

        virtual void reset() {}

        virtual std::size_t get_content_length() { return 0; }
        
        virtual bool can_parse(ContentType contentType) = 0;

        // 解析
        virtual bool parse(HttpPacket *packet) = 0;
    };

}

#endif