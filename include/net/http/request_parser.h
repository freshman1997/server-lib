#ifndef __NET_HTTP_REQUEST_PARSER_H__
#define __NET_HTTP_REQUEST_PARSER_H__
#include "net/http/packet_parser.h"

namespace net::http 
{
    class HttpRequest;

    class HttpRequestParser : public HttpPacketParser
    {
    public:
        HttpRequestParser() {}
        HttpRequestParser(HttpPacket *packet) : HttpPacketParser(packet)
        {}

        virtual bool parse_header(Buffer &buff);

    private:
        bool parse_method(Buffer &buff);
        bool parse_url(Buffer &buff);
    };
}

#endif