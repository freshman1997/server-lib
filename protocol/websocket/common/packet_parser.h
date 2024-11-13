#ifndef __NET_WEBSOCKET_COMMON_PACKET_PARSER_H__
#define __NET_WEBSOCKET_COMMON_PACKET_PARSER_H__

#include "buffer/buffer.h"
namespace net::websocket 
{
    class WebSocketConnection;

    class WebSocketPacketParser
    {
    public:
        static bool unpack(WebSocketConnection *conn);

        static bool pack(WebSocketConnection *conn, Buffer *buff);
    };
}

#endif
