#ifndef __NET_HTTP_URL_ENCODED_CONTENT_PARSER_H__
#define __NET_HTTP_URL_ENCODED_CONTENT_PARSER_H__
#include "content/content_parser.h"

namespace net::http
{
    class UrlEncodedContentParser final : public ContentParser
    {
    public:
        bool can_parse(ContentType contentType) override;

        bool parse(HttpPacket *packet) override;
    };
}

#endif