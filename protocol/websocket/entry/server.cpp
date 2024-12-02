#include "server.h"
#include "net/acceptor/acceptor.h"
#include "net/acceptor/tcp_acceptor.h"
#include "net/event/event_loop.h"
#include "net/poller/epoll_poller.h"
#include "net/poller/select_poller.h"
#include "net/socket/socket.h"
#include "timer/wheel_timer_manager.h"
#include "net/connection/connection.h"
#include "../common/websocket_connection.h"
#include "data_handler.h"
#include "../common/websocket_config.h"

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

    bool WebSocketServer::init(int port)
    {
        Socket *sock = new Socket("", port);
        sock->set_none_block(true);
        if (!sock->valid()) {
            delete sock;
            return false;
        }
        
        sock->set_reuse(true);
        sock->set_no_deylay(true);
        sock->set_keep_alive(true);
        if (!sock->bind()) {
            delete sock;
            return false;
        }

        TcpAcceptor *acceptor = new TcpAcceptor(sock);
        if (!acceptor->listen()) {
            delete acceptor;
            return false;
        }

    #ifdef unix
        poller_ = new EpollPoller;
    #else
        poller_ = new SelectPoller;
    #endif

        if (!poller_->init()) {
            delete poller_;
            delete acceptor;
            return false;
        }

        timer_manager_ = new timer::WheelTimerManager;
        loop_ = new EventLoop(poller_, timer_manager_);
        
        acceptor->set_connection_handler(this);
        acceptor->set_event_handler(loop_);

        return WebSocketConfigManager::get_instance()->init(true);
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
        if (connected_urls_.find(wsConn->get_url()) != connected_urls_.end()) {
            wsConn->close();
            return;
        }

        connected_urls_.insert(wsConn->get_url());
        wsConn->try_set_heartbeat_timer(timer_manager_);
        if (data_handler_) {
            data_handler_->on_connected(wsConn);
        }
    }

    void WebSocketServer::on_receive_packet(WebSocketConnection *wsConn, Buffer *buff)
    {
        if (data_handler_) {
            data_handler_->on_data(wsConn, buff);
        }
    }

    void WebSocketServer::on_close(WebSocketConnection *wsConn)
    {
        if (data_handler_) {
            data_handler_->on_close(wsConn);
        }
        connected_urls_.erase(wsConn->get_url());
        connections_.erase(wsConn->get_native_connection());
    }
}
