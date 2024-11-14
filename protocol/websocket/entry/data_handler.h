#ifndef __NET_WEBSOCKET_COMMON_DATA_HANDLER_H__
#define __NET_WEBSOCKET_COMMON_DATA_HANDLER_H__
#include "buffer/linked_buffer.h"

namespace net::websocket 
{
    class WebSocketConnection;

    class WebSocketDataHandler
    {
    public:
        virtual void on_data(WebSocketConnection *wsConn, const Buffer *buff) = 0;
    };
}

#endif
