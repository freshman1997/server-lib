#ifndef __NET_WEBSOCKET_COMMON_HANDLER_H__
#define __NET_WEBSOCKET_COMMON_HANDLER_H__
#include "buffer/byte_buffer.h"

namespace yuan::net::websocket
{
    class WebSocketConnection;

    class[[deprecated("Use WebSocketConnection callbacks (on_data, on_connected_cb, on_close_cb) instead")]] WebSocketHandler
    {
    public:
        virtual void on_connected(WebSocketConnection *conn) = 0;

        virtual void on_receive_packet(WebSocketConnection * conn, const ::yuan::buffer::ByteBuffer &buff) = 0;

        virtual void on_close(WebSocketConnection *conn) = 0;
    };
}

#endif
