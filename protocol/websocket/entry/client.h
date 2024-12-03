#ifndef __NET_WEBSOCKET_ENTRY_CLIENT_H__
#define __NET_WEBSOCKET_ENTRY_CLIENT_H__
#include "data_handler.h"
#include "../common/handler.h"
#include "net/connector/connector.h"
#include "net/event/event_loop.h"
#include "net/handler/connection_handler.h"
#include "net/handler/connector_handler.h"
#include "net/poller/poller.h"
#include "net/socket/inet_address.h"
#include "timer/timer_manager.h"

namespace net::websocket 
{
    class WebSocketClient : public WebSocketHandler, public ConnectorHandler
    {
    public:
        enum class State
        {
            connecting_,
            connected_,
            closing_,
            closed_,
            connect_timeout_,
        };

    public:
        WebSocketClient();
        ~WebSocketClient();

        bool init();

        bool connect(const InetAddress &addr, const std::string &url = "/");

        void set_data_handler(WebSocketDataHandler *handler);

        void run();

        void exit();

    protected:
        void on_connected(WebSocketConnection *conn);

        void on_receive_packet(WebSocketConnection *conn, Buffer *buff);

        void on_close(WebSocketConnection *conn);

    protected:
        virtual void on_connect_failed(Connection *conn);

        virtual void on_connect_timeout(Connection *conn);

        virtual void on_connected_success(Connection *conn);

    private:
        State state_;
        WebSocketDataHandler *data_handler_;
        WebSocketConnection *conn_;
        timer::TimerManager *timer_manager_;
        Poller *poller_;
        EventLoop *loop_;
        std::string url_;
        std::shared_ptr<Connector> connector_;
        std::shared_ptr<SSLModule> ssl_module_;
    };
}

#endif