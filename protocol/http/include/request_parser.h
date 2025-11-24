#ifndef __NET_HTTP_REQUEST_PARSER_H__
#define __NET_HTTP_REQUEST_PARSER_H__
#include "packet_parser.h"

namespace yuan::net::http 
{
    class HttpRequest;

    class HttpRequestParser : public HttpPacketParser
    {
    public:
        HttpRequestParser() {}
        HttpRequestParser(HttpPacket *packet) : HttpPacketParser(packet)
        {}

        virtual bool parse_header(buffer::BufferReader &reader);

    private:
        bool parse_method(buffer::BufferReader &reader);
        bool parse_url(buffer::BufferReader &reader);
    };
}

#endif