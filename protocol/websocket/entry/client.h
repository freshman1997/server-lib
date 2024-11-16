#ifndef __NET_WEBSOCKET_ENTRY_CLIENT_H__
#define __NET_WEBSOCKET_ENTRY_CLIENT_H__
#include "data_handler.h"
#include "../common/handler.h"
#include "net/event/event_loop.h"
#include "net/handler/connection_handler.h"
#include "net/poller/poller.h"
#include "net/socket/inet_address.h"
#include "timer/timer.h"
#include "timer/timer_manager.h"

namespace net::websocket 
{
    class WebSocketClient : public WebSocketHandler, public ConnectionHandler
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

        bool create(const InetAddress &addr);

        void on_connect_timeout(timer::Timer *timer);

        void set_data_handler(WebSocketDataHandler *handler);

        void run();

        void exit();

    protected:
        void on_connected(WebSocketConnection *conn);

        void on_receive_packet(WebSocketConnection *conn, Buffer *buff);

        void on_close(WebSocketConnection *conn);

    protected:
        virtual void on_connected(Connection *conn);

        virtual void on_error(Connection *conn);

        virtual void on_read(Connection *conn);

        virtual void on_write(Connection *conn);

        virtual void on_close(Connection *conn);

    private:
        State state_;
        WebSocketDataHandler *data_handler_;
        WebSocketConnection *conn_;
        timer::TimerManager *timer_manager_;
        Poller *poller_;
        EventLoop *loop_;
        timer::Timer *conn_timer_;
    };
}

#endif