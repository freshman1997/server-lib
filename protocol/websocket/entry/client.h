#ifndef __NET_WEBSOCKET_ENTRY_CLIENT_H__
#define __NET_WEBSOCKET_ENTRY_CLIENT_H__
#include "../common/handler.h"

namespace net::websocket 
{
    class WebSocketClient : public WebSocketHandler
    {
    public:
        void on_connected(WebSocketConnection *conn);

        void on_receive_packet(WebSocketConnection *conn, const Buffer *buff);

        void on_close(WebSocketConnection *conn);
    };
}

#endif