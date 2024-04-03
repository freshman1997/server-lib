#ifndef __NET_HTTP_JSON_CONTENT_PARSER_H__
#define __NET_HTTP_JSON_CONTENT_PARSER_H__
#include "net/http/content/content_parser.h"

namespace net::http 
{
    class JsonContentParser : public ContentParser
    {
    public:
        
        virtual bool can_parse(const content_type contentType);

        // 解析
        virtual bool parse(HttpPacket *packet);
    };
}

#endif