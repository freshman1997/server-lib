#ifndef __NET_HTTP_CONTENT_PARSER_FACTORY_H__
#define __NET_HTTP_CONTENT_PARSER_FACTORY_H__
#include <unordered_map>
#include "net/http/content_type.h"

namespace net::http 
{
    class HttpPacket;
    class ContentParser;

    class ContentParserFactory
    {
    public:
        ContentParserFactory();
        ~ContentParserFactory();

        bool parse_content(HttpPacket *packet);

    private:
        std::unordered_map<content_type, ContentParser *> parsers;
    };
}

#endif