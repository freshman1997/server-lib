#ifndef __NET_WEBSOCKET_ENTRY_SERVER_H__
#define __NET_WEBSOCKET_ENTRY_SERVER_H__
#include <memory>

namespace yuan::net::websocket 
{
    class WebSocketDataHandler;
    class WebSocketServer
    {
    public:
        WebSocketServer();
        ~WebSocketServer();

    public:
        void set_data_handler(WebSocketDataHandler *handler);

        bool init(int port);

        void serve();

    private:
        class ServerData;
        std::unique_ptr<ServerData> data_;
    };
}

#endif
