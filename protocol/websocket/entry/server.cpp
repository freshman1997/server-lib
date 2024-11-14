#include "server.h"
#include "net/acceptor/acceptor.h"
#include "net/acceptor/tcp_acceptor.h"
#include "net/event/event_loop.h"
#include "net/poller/epoll_poller.h"
#include "net/socket/socket.h"
#include "timer/wheel_timer_manager.h"
#include "net/connection/connection.h"
#include "../common/websocket_connection.h"
#include "data_handler.h"

namespace net::websocket
{
    WebSocketServer::WebSocketServer()
    {
        data_handler_ = nullptr;
        loop_ = nullptr;
        timer_manager_ = nullptr;
    }

    WebSocketServer::~WebSocketServer()
    {

    }

    bool WebSocketServer::init()
    {
        Socket *sock = new Socket("", 12211);
        sock->set_none_block(true);
        if (!sock->valid()) {
            delete sock;
            return false;
        }

        if (!sock->bind()) {
            delete sock;
            return false;
        }

        if (!sock->listen()) {
            delete sock;
            return false;
        }

        EpollPoller poller;
        if (!poller.init()) {
            return false;
        }

        timer::WheelTimerManager timerManager;
        EventLoop loop(&poller, &timerManager);
        loop_ = &loop;

        TcpAcceptor *acceptor = new TcpAcceptor(sock);
        acceptor->set_connection_handler(this);
        acceptor->set_event_handler(&loop);

        return true;
    }

    void WebSocketServer::serve()
    {
        if (loop_) {
            loop_->loop();
        }
    }

    void WebSocketServer::on_connected(Connection *conn)
    {
        auto it = connections_.find(conn);
        if (it != connections_.end()) {
            conn->close();
        } else {
            WebSocketConnection *wsConn = new WebSocketConnection(conn);
            wsConn->set_handler(this);
            connections_[conn] = wsConn;
        }
    }

    void WebSocketServer::on_error(Connection *conn) {}

    void WebSocketServer::on_read(Connection *conn) {}

    void WebSocketServer::on_write(Connection *conn) {}

    void WebSocketServer::on_close(Connection *conn)
    {
        connections_.erase(conn);
    }

    void WebSocketServer::on_connected(WebSocketConnection *wsConn)
    {
        // handshake done
    }

    void WebSocketServer::on_receive_packet(WebSocketConnection *wsConn, const Buffer *buff)
    {
        if (data_handler_) {
            data_handler_->on_data(wsConn, buff);
        }
    }

    void WebSocketServer::on_close(WebSocketConnection *wsConn)
    {
        connections_.erase(wsConn->get_native_connection());
    }
}
