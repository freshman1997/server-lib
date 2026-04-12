#ifndef __NET_HTTP_RESPONSE_PARSER_H__
#define __NET_HTTP_RESPONSE_PARSER_H__
#include "packet_parser.h"

namespace yuan::net::http 
{
    class HttpResponseParser : public HttpPacketParser
    {
    public:
        HttpResponseParser() {}
        HttpResponseParser(HttpPacket *packet) : HttpPacketParser(packet)
        {}

        virtual bool parse_header(::yuan::buffer::ByteBuffer &buff);

    private:
        bool parse_status(::yuan::buffer::ByteBuffer &buff);
        bool parse_status_desc(::yuan::buffer::ByteBuffer &buff);
    };
}

#endif
