#ifndef __NET_WEBSOCKET_ENTRY_SERVER_H__
#define __NET_WEBSOCKET_ENTRY_SERVER_H__
#include "../common/handler.h"
#include "net/event/event_loop.h"
#include "timer/timer_manager.h"
#include <unordered_map>

namespace net::websocket 
{
    class WebSocketDataHandler;
    class WebSocketServer : public WebSocketHandler, public ConnectionHandler
    {
    public:
        WebSocketServer();
        ~WebSocketServer();

        bool init();

    public:
        virtual void on_connected(Connection *conn);

        virtual void on_error(Connection *conn);

        virtual void on_read(Connection *conn);

        virtual void on_write(Connection *conn);

        virtual void on_close(Connection *conn);

    public:
        void on_connected(WebSocketConnection *wsConn);

        void on_receive_packet(WebSocketConnection *wsConn, const Buffer *buff);

        void on_close(WebSocketConnection *wsConn);

    public:
        void set_data_handler(WebSocketDataHandler *handler)
        {
            data_handler_ = handler;
        }

        void serve();

    private:
        WebSocketDataHandler *data_handler_;
        Poller *poller_;
        EventLoop *loop_;
        timer::TimerManager *timer_manager_;
        std::unordered_map<Connection *, WebSocketConnection *> connections_;
    };
}

#endif
