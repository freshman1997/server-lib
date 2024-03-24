#ifndef __NET_HTTP_PARSERS_TEXT_PARSER_H__
#define __NET_HTTP_PARSERS_TEXT_PARSER_H__
#include "../content_parser.h"

namespace net::http 
{
    class TextContentParser : public ContentParser
    {
    public:
        // 检查是否可以解析
        virtual bool can_parse(const content_type contentType);

        // 解析
        virtual bool parse(HttpRequest *req);
    };
}

#endif