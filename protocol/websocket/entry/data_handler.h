#ifndef __NET_WEBSOCKET_COMMON_DATA_HANDLER_H__
#define __NET_WEBSOCKET_COMMON_DATA_HANDLER_H__
#include "buffer/linked_buffer.h"

namespace yuan::net::websocket 
{
    class WebSocketConnection;

    class WebSocketDataHandler
    {
    public:
        virtual void on_connected(WebSocketConnection *wsConn) = 0;

        virtual void on_data(WebSocketConnection *wsConn, const buffer::Buffer *buff) = 0;

        virtual void on_close(WebSocketConnection *wsConn) = 0;
    };
}

#endif
