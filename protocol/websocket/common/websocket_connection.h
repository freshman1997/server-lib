#ifndef __NET_WEBSOCKET_COMMON_WEB_SOCKET_CONNECTION_H__
#define __NET_WEBSOCKET_COMMON_WEB_SOCKET_CONNECTION_H__
#include "buffer/linked_buffer.h"
#include "net/connection/connection.h"
#include "net/handler/connection_handler.h"
#include "net/socket/inet_address.h"
#include <memory>

namespace net::websocket 
{
    class WebSocketHandler;
    class ProtoChunk;

    class WebSocketConnection
    {
        friend class WebSocketPacketParser;
    public:
        enum class WorkMode
        {
            client_,
            server_
        };

    public:
        WebSocketConnection(Connection *conn);
        ~WebSocketConnection();

    public:
        void send(Buffer *buf);

        void close();

        void set_handler(WebSocketHandler *handler);

        net::Connection * get_native_connection();

    private:
        void free_self();

        std::vector<Buffer *> * get_output_buffers();

        std::vector<ProtoChunk> * get_input_chunks();
        
    private:
        class ConnData;
        std::unique_ptr<ConnData> data_;
    };


}

#endif