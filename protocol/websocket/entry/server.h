#ifndef __NET_WEBSOCKET_ENTRY_SERVER_H__
#define __NET_WEBSOCKET_ENTRY_SERVER_H__
#include <memory>

#include "coroutine/task.h"
#include "net/async/async_listener_host.h"
#include "net/async/async_connection_context.h"
#include "net/runtime/network_runtime.h"

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
        bool init(int port, NetworkRuntime &runtime);

        void serve();

        void stop();

    private:
        coroutine::Task<void> handle_connection(net::AsyncConnectionContext ctx);

    private:
        class ServerData;
        std::unique_ptr<ServerData> data_;
    };
}
#endif
