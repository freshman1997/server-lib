#include "client.h"
#include "net/connection/connection.h"
#include "../common/websocket_connection.h"
#include "../common/websocket_config.h"
#include "net/connection/tcp_connection.h"
#include "net/poller/epoll_poller.h"
#include "net/poller/select_poller.h"
#include "net/socket/socket.h"
#include "timer/wheel_timer_manager.h"
#include "timer/timer_util.hpp"
#include <cassert>

namespace net::websocket
{
    WebSocketClient::WebSocketClient() : state_(State::connecting_), data_handler_(nullptr), conn_(nullptr), timer_manager_(nullptr), poller_(nullptr), loop_(nullptr), conn_timer_(nullptr)
    {
    }

    WebSocketClient::~WebSocketClient()
    {
        exit();
    }

    bool WebSocketClient::create(const InetAddress &addr, const std::string &url)
    {
        Socket *sock = new Socket(addr.get_ip().c_str(), addr.get_port());
        sock->set_none_block(true);
        if (!sock->valid()) {
            delete sock;
            return false;
        }
        
        sock->set_none_block(true);
        if (!sock->connect()) {
            delete sock;
            return false;
        }
    #ifdef unix
        poller_ = new EpollPoller;
    #else
        poller_ = new SelectPoller;
    #endif
        if (!poller_->init()) {
            delete sock;
            delete poller_;
            return false;
        }

        Connection *conn = new TcpConnection(sock);
        timer_manager_ = new timer::WheelTimerManager;
        loop_ = new EventLoop(poller_, timer_manager_);

        conn->set_connection_handler(this);
        loop_->update_channel(conn->get_channel());
        conn->set_event_handler(loop_);

        conn_timer_ = timer::TimerUtil::build_timeout_timer(timer_manager_, 10 * 1000, this, &WebSocketClient::on_connect_timeout);
        url_ = url;

        return WebSocketConfigManager::get_instance()->init(false);
    }

    void WebSocketClient::on_connect_timeout(timer::Timer *timer)
    {
        state_ = State::connect_timeout_;
        on_close((Connection *)nullptr);
    }

    void WebSocketClient::set_data_handler(WebSocketDataHandler *handler)
    {
        data_handler_ = handler;
    }

    void WebSocketClient::on_connected(Connection *conn) 
    {
        if (conn_) {
            conn->close();
            return;
        }

        WebSocketConnection *wsConn = new WebSocketConnection(WebSocketConnection::WorkMode::client_);
        wsConn->set_handler(this);
        wsConn->set_url(url_);
        wsConn->on_created(conn);
        state_ = State::connected_;
    }

    void WebSocketClient::on_error(Connection *conn) {}

    void WebSocketClient::on_read(Connection *conn) {}

    void WebSocketClient::on_write(Connection *conn) {}

    void WebSocketClient::on_close(Connection *conn) 
    {
        on_close(conn_);
    }

    void WebSocketClient::on_connected(WebSocketConnection *conn)
    {
        conn_ = conn;
        if (conn_timer_) {
            conn_timer_->cancel();
            conn_timer_ = nullptr;
        }

        if (timer_manager_) {
            conn_->try_set_heartbeat_timer(timer_manager_);
        }

        conn_->set_handler(this);
        if (data_handler_) {
            data_handler_->on_connected(conn);
        }
    }

    void WebSocketClient::on_receive_packet(WebSocketConnection *conn, Buffer *buff)
    {
        if (data_handler_) {
            data_handler_->on_data(conn, buff);
        }
    }

    void WebSocketClient::on_close(WebSocketConnection *conn)
    {
        state_ = State::closed_;
        if (data_handler_ && conn) {
            data_handler_->on_close(conn);
        }
        conn_ = nullptr;

        if (conn_timer_) {
            conn_timer_->cancel();
            conn_timer_ = nullptr;
        }

        if (loop_) {
            loop_->quit();
        }
    }

    void WebSocketClient::run()
    {
        assert(loop_);
        loop_->loop();
    }

    void WebSocketClient::exit()
    {
        state_ = State::closed_;
        if (conn_) {
            conn_->close();
            conn_ = nullptr;
        }

        if (conn_) {
            conn_->close();
            conn_ = nullptr;
        }

        if (conn_timer_) {
            conn_timer_->cancel();
            conn_timer_ = nullptr;
        }

        if (loop_) {
            loop_->quit();
            delete loop_;
            loop_ = nullptr;
        }

        if (poller_) {
            delete poller_;
            poller_ = nullptr;
        }

        if (timer_manager_) {
            delete timer_manager_;
            timer_manager_ = nullptr;
        }
    }
}