#ifndef __NET_WEBSOCKET_COMMON_HANDLER_H__
#define __NET_WEBSOCKET_COMMON_HANDLER_H__
#include "buffer/linked_buffer.h"

namespace net::websocket 
{
    class WebSocketConnection;

    class WebSocketHandler
    {
    public:
        virtual void on_connected(WebSocketConnection *conn) = 0;

        virtual void on_receive_packet(WebSocketConnection *conn, const Buffer *buff) = 0;

        virtual void on_close(WebSocketConnection *conn) = 0;
    };
}

#endif
