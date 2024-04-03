#ifndef __NET_HTTP_RESPONSE_PARSER_H__
#define __NET_HTTP_RESPONSE_PARSER_H__
#include "net/http/packet_parser.h"

namespace net::http 
{
    class HttpResponseParser : public HttpPacketParser
    {
    public:
        HttpResponseParser() {}
        HttpResponseParser(HttpPacket *packet) : HttpPacketParser(packet)
        {}

        virtual bool parse_header(Buffer &buff);

    private:
        bool parse_status(Buffer &buff);
        bool parse_status_desc(Buffer &buff);
    };
}

#endif