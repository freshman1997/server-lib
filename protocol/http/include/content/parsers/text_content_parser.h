#ifndef __NET_HTTP_PARSERS_TEXT_PARSER_H__
#define __NET_HTTP_PARSERS_TEXT_PARSER_H__
#include "../content_parser.h"

namespace net::http 
{
    class TextContentParser final : public ContentParser
    {
    public:
        // 检查是否可以解析
        bool can_parse(ContentType contentType) override;

        // 解析
        bool parse(HttpPacket *packet) override;
    };
}

#endif