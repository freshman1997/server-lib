#ifndef __NET_WEBSOCKET_ENTRY_CLIENT_H__
#define __NET_WEBSOCKET_ENTRY_CLIENT_H__
#include "data_handler.h"
#include "coroutine/task.h"
#include "net/async/async_client_session.h"
#include "net/socket/inet_address.h"
#include <memory>

namespace yuan::net::websocket
{
    class WebSocketClient
    {
    public:
        WebSocketClient();
        ~WebSocketClient();

        bool init();

        bool connect(const InetAddress &addr, const std::string &url = "/");

        void set_data_handler(WebSocketDataHandler *handler);

        void run();

        void exit();

    private:
        coroutine::Task<void> run_async(const InetAddress &addr, const std::string &url);

    private:
        class ClientData;
        std::unique_ptr<ClientData> data_;
    };
}
#endif