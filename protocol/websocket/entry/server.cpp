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
#include "../common/handler.h"

#include <memory>
#include <unordered_set>
#include <unordered_map>
#include <iostream>

#if defined (WS_USE_SSL)
#include "net/secuity/openssl.h"
#endif

namespace net::websocket
{
    class WebSocketServer::ServerData : public WebSocketHandler, public ConnectionHandler
    {
    public:
        ServerData()
        {
            data_handler_ = nullptr;
            poller_ = nullptr;
            loop_ = nullptr;
            timer_manager_ = nullptr;
        }

        ~ServerData() 
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

        bool init(int port)
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

        #if defined (WS_USE_SSL)
            ssl_module_ = std::make_shared<OpenSSLModule>();
            if (!ssl_module_->init("./ssl/ca.crt", "./ssl/ca.key", SSLHandler::SSLMode::acceptor_)) {
                if (auto msg = ssl_module_->get_error_message()) {
                    std::cerr << msg->c_str() << '\n';
                }
                return false;
            }
            acceptor->set_ssl_module(ssl_module_);
        #endif

            timer_manager_ = new timer::WheelTimerManager;
            loop_ = new EventLoop(poller_, timer_manager_);
            
            acceptor->set_connection_handler(this);
            acceptor->set_event_handler(loop_);

            return WebSocketConfigManager::get_instance()->init(true);
        }

        virtual void on_connected(Connection *conn)
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

        virtual void on_error(Connection *conn) {}

        virtual void on_read(Connection *conn) {}

        virtual void on_write(Connection *conn) {}

        virtual void on_close(Connection *conn)
        {
            connections_.erase(conn);
        }

        void on_connected(WebSocketConnection *wsConn)
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

        void on_receive_packet(WebSocketConnection *wsConn, Buffer *buff)
        {
            if (data_handler_) {
                data_handler_->on_data(wsConn, buff);
            }
        }

        void on_close(WebSocketConnection *wsConn)
        {
            if (data_handler_) {
                data_handler_->on_close(wsConn);
            }
            connected_urls_.erase(wsConn->get_url());
            connections_.erase(wsConn->get_native_connection());
        }

    public:
        WebSocketDataHandler *data_handler_;
        Poller *poller_;
        EventLoop *loop_;
        timer::TimerManager *timer_manager_;
        std::unordered_map<Connection *, WebSocketConnection *> connections_;
        std::unordered_set<std::string> connected_urls_;
        std::shared_ptr<SSLModule> ssl_module_;
    };

    WebSocketServer::WebSocketServer() : data_(std::make_unique<WebSocketServer::ServerData>())
    {
    }

    WebSocketServer::~WebSocketServer() {}

    bool WebSocketServer::init(int port)
    {
        return data_->init(port);
    }

    void WebSocketServer::serve()
    {
        if (data_->loop_) {
            data_->loop_->loop();
        }
    }

    void WebSocketServer::set_data_handler(WebSocketDataHandler *handler)
    {
        data_->data_handler_ = handler;
    }
}
