#ifndef NET_HTTP_JSON_CONTENT_PARSER_H_
#define NET_HTTP_JSON_CONTENT_PARSER_H_
#include "content/content_parser.h"

namespace net::http 
{
    class JsonContentParser final : public ContentParser
    {
    public:
        
        bool can_parse(ContentType contentType) override;

        // 解析
        bool parse(HttpPacket *packet) override;
    };
}

#endif