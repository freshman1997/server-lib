#ifndef __NET_HTTP_CONTENT_PARSER_FACTORY_H__
#define __NET_HTTP_CONTENT_PARSER_FACTORY_H__

namespace net::http 
{
    class HttpRequest;

    class ContentParserFactory
    {
    public:
        bool parse_content(HttpRequest *req);
    };
}

#endif