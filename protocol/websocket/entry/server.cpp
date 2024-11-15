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
#include <iostream>

namespace net::websocket
{
    WebSocketServer::WebSocketServer()
    {
        data_handler_ = nullptr;
        poller_ = nullptr;
        loop_ = nullptr;
        timer_manager_ = nullptr;
    }

    WebSocketServer::~WebSocketServer()
    {
        if (timer_manager_) {
            delete timer_manager_;
            timer_manager_ = nullptr;
        }

        if (poller_) {
            delete poller_;
            poller_ = nullptr;
        }

        if (loop_) {
            delete loop_;
            loop_ = nullptr;
        }
    }

    bool WebSocketServer::init()
    {
        Socket *sock = new Socket("", 12211);
        sock->set_none_block(true);
        if (!sock->valid()) {
            delete sock;
            return false;
        }
        
        sock->set_reuse(true);
        sock->set_none_block(true);
        if (!sock->bind()) {
            delete sock;
            return false;
        }

        TcpAcceptor *acceptor = new TcpAcceptor(sock);
        if (!acceptor->listen()) {
            delete acceptor;
            return false;
        }

        poller_ = new EpollPoller;
        if (!poller_->init()) {
            delete poller_;
            return false;
        }

        timer_manager_ = new timer::WheelTimerManager;
        loop_ = new EventLoop(poller_, timer_manager_);
        
        acceptor->set_connection_handler(this);
        acceptor->set_event_handler(loop_);

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
            WebSocketConnection *wsConn = new WebSocketConnection();
            wsConn->set_handler(this);
            connections_[conn] = wsConn;
            wsConn->on_created(conn);
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
