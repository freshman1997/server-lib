#include "client.h"
#include "net/connection/connection.h"
#include "../common/websocket_connection.h"
#include "../common/websocket_config.h"
#include "net/connector/tcp_connector.h"
#include "net/poller/epoll_poller.h"
#include "net/poller/select_poller.h"
#include "net/socket/socket.h"
#include "timer/wheel_timer_manager.h"
#include "timer/timer_util.hpp"

#if defined (WS_USE_SSL)
#include "net/secuity/openssl.h"
#endif

#include <cassert>
#include <iostream>

namespace net::websocket
{
    WebSocketClient::WebSocketClient() : state_(State::connecting_), data_handler_(nullptr), conn_(nullptr), timer_manager_(nullptr), poller_(nullptr), loop_(nullptr)
    {
    
    }

    WebSocketClient::~WebSocketClient()
    {
        exit();
    }

    bool WebSocketClient::init()
    {
        if (!WebSocketConfigManager::get_instance()->init(false)) {
            return false;
        }

    #if defined (WS_USE_SSL)
        ssl_module_ = std::make_shared<OpenSSLModule>();
        if (!ssl_module_->init("./ssl/ca.crt")) {
            if (auto msg = ssl_module_->get_error_message()) {
                std::cerr << msg->c_str() << '\n';
            }
            return false;
        }
    #endif

    #ifdef unix
        poller_ = new EpollPoller;
    #else
        poller_ = new SelectPoller;
    #endif
        if (!poller_->init()) {
            delete poller_;
            return false;
        }

        timer_manager_ = new timer::WheelTimerManager;
        loop_ = new EventLoop(poller_, timer_manager_);

        return true;
    }

    bool WebSocketClient::connect(const InetAddress &addr, const std::string &url)
    {
        url_ = url;

        connector_ = std::make_shared<TcpConnector>();
        connector_->set_data(timer_manager_, this, loop_);
        
        if (ssl_module_) {
            connector_->set_ssl_module(ssl_module_);
        }

        return connector_->connect(addr);
    }

    void WebSocketClient::set_data_handler(WebSocketDataHandler *handler)
    {
        data_handler_ = handler;
    }

    void WebSocketClient::on_connect_failed(Connection *conn)
    {
        on_close(conn_);
    }

    void WebSocketClient::on_connect_timeout(Connection *conn)
    {
        on_close(conn_);
    }

    void WebSocketClient::on_connected_success(Connection *conn)
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

    void WebSocketClient::on_connected(WebSocketConnection *conn)
    {
        conn_ = conn;
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