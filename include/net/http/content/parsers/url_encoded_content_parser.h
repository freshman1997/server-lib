#ifndef __NET_HTTP_URL_ENCODED_CONTENT_PARSER_H__
#define __NET_HTTP_URL_ENCODED_CONTENT_PARSER_H__
#include "net/http/content/content_parser.h"

namespace net::http
{
    class UrlEncodedContentParser : public ContentParser
    {
    public:
        virtual bool can_parse(ContentType contentType);

        virtual bool parse(HttpPacket *packet);
    };
}

#endif