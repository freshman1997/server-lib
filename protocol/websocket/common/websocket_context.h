#ifndef __NET_WEBSOCKET_COMMON_WEBSOCKET_CONTEXT_H__
#define __NET_WEBSOCKET_COMMON_WEBSOCKET_CONTEXT_H__

namespace net::websocket 
{
    class Connection;

    enum class WebSocketState : char
    {
        init = 0,
        handshake_done,
    };

    class WebSocketContext
    {
    public:
        void on_connected(Connection *);

    private:
    };

}

#endif