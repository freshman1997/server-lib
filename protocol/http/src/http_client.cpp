#include <iostream>
#include "net/connection/connection.h"
#include "net/connection/tcp_connection.h"
#include "net/poller/epoll_poller.h"
#include "net/poller/select_poller.h"
#include "context.h"
#include "http_client.h"
#include "net/socket/socket.h"
#include "ops/option.h"
#include "session.h"
#include "timer/wheel_timer_manager.h"
#include "net/secuity/openssl.h"

namespace yuan::net::http 
{
    HttpClient::HttpClient()
    {
        port_ = 80;
        host_name_.clear();
        session_ = nullptr;
        ccb_ = nullptr;
        rcb_ = nullptr;
        conn_timer_ = nullptr;
        ssl_module_ = nullptr;
    }

    HttpClient::~HttpClient()
    {
        if (session_) {
            delete session_;
        }

        if (conn_timer_) {
            conn_timer_->cancel();
        }

        ev_loop_->quit();
    }

    void HttpClient::on_connected(Connection *conn)
    {
        HttpSessionContext *ctx = new HttpSessionContext(conn);
        ctx->set_mode(Mode::client);
        session_ = new HttpSession((uint64_t)conn, ctx, timer_manager_);
        if (ccb_) {
            if (conn_timer_) {
                conn_timer_->cancel();
                conn_timer_ = nullptr;
            }

            ccb_(ctx->get_request());
        }
    }

    void HttpClient::on_error(Connection *conn)
    {
        exit();
    }

    void HttpClient::on_read(Connection *conn)
    {
        HttpSessionContext *context = session_->get_context();
        if (!context->parse()) {
            if (context->has_error()) {
                exit();
                return;
            }
            return;
        }

        if (context->has_error()) {
            exit();
            return;
        }

        if (context->is_downloading()) {
            return;
        }

        if (context->is_completed()) {
            (void)context->try_parse_request_content();

            if (!context->is_completed()) {
                return;
            }

            if (rcb_) {
                rcb_(context->get_request(), context->get_response());
            }
        }
        session_->reset_timer();
    }

    void HttpClient::on_write(Connection *conn)
    {
        
    }

    void HttpClient::on_close(Connection *conn)
    {
        exit();
    }

    bool HttpClient::query(const std::string &url)
    {
        if (url.find("://") == std::string::npos) {
            return false;
        }
        size_t pos = url.find("://");
        std::string protocol = url.substr(0, pos);
        if (protocol != "http" && protocol != "https") {
            return false;
        }

        std::string rest = url.substr(pos + 3);
        size_t port_pos = rest.find(":");
        if (port_pos != std::string::npos) {
            std::string port_str = rest.substr(port_pos + 1);
            port_ = std::stoi(port_str);
            if (port_ <= 0 || port_ > 65535) {
                return false;
            }
        }

        size_t path_pos = rest.find("/");
        if (path_pos != std::string::npos) {
            std::string path = rest.substr(path_pos);
            if (path.empty()) {
                return false;
            }
        } else {
            if (port_pos != std::string::npos) {
                path_pos = port_pos;
            } else {
                path_pos = rest.size();
            }
        }

        host_name_ = rest.substr(0, port_pos > 0 ? port_pos : path_pos);

        return true;
    }

    bool HttpClient::connect(connected_callback ccb, request_function rcb)
    {
        if (!ccb || !rcb) {
            std::cout << "must set callback!!\n";
            return false;
        }

        InetAddress addr{ host_name_.c_str(), port_ };
        if (addr.get_ip().empty()) {
            std::cout << "get ip failed!!\n";
            return false;
        }

        if (addr.get_port() <= 0 || addr.get_port() > 65535) {
            std::cout << "port is invalid!!\n";
            return false;
        }
        
        net::Socket *sock = new net::Socket(addr.get_ip().c_str(), addr.get_port());
        if (!sock->valid()) {
            std::cout << "create socket fail!!\n";
            return false;
        }

        sock->set_none_block(true);
        if (!sock->connect()) {
            std::cout << " connect failed " << std::endl;
            return false;
        }

        config::load_config();

        Connection *conn = new TcpConnection(sock);

    #ifdef HTTP_USE_SSL
        ssl_module_ = std::make_shared<OpenSSLModule>();
        if (!ssl_module_->init("ca-cert.pem", "ca-key.pem", SSLHandler::SSLMode::connector_)) {
            if (auto msg = ssl_module_->get_error_message()) {
                std::cerr << msg->c_str() << '\n';
            }
            conn->abort();
            return false;
        }

        conn->set_ssl_handler(ssl_module_->create_handler(sock->get_fd(), SSLHandler::SSLMode::connector_));
    #endif

        timer::WheelTimerManager manager;
        conn_timer_ = manager.timeout(config::connection_idle_timeout, this);

#ifdef _WIN32
        SelectPoller poller;
#else
        EpollPoller poller;
#endif
        net::EventLoop loop(&poller, &manager);

        conn->set_connection_handler(this);
        loop.update_channel(conn->get_channel());
        conn->set_event_handler(&loop);

        rcb_ = rcb;
        ccb_ = ccb;
        ev_loop_ = &loop;
        timer_manager_ = &manager;
        loop.loop();
        
        return true;
    }

    void HttpClient::on_timer(timer::Timer *timer)
    {
        std::cout << "connect timeout, close connection now!\n";
        exit();
    }

    void HttpClient::exit()
    {
        if (ev_loop_) {
            ev_loop_->quit();
        }
    }
}