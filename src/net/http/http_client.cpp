#include <iostream>
#include "net/base/connection/connection.h"
#include "net/base/connection/tcp_connection.h"
#include "net/base/poller/epoll_poller.h"
#include "net/http/context.h"
#include "net/http/http_client.h"
#include "net/base/socket/socket.h"
#include "net/http/ops/option.h"
#include "net/http/session.h"
#include "singleton/singleton.h"
#include "timer/wheel_timer_manager.h"

namespace net::http 
{
    HttpClient::HttpClient()
    {
        session_ = nullptr;
        ccb_ = nullptr;
        rcb_ = nullptr;
        conn_timer_ = nullptr;
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
        delete this;
    }

    void HttpClient::on_read(Connection *conn)
    {
        HttpSessionContext *context = session_->get_context();
        if (!context->parse()) {
            if (context->has_error()) {
                delete this;
                return;
            }
            return;
        }

        if (context->has_error()) {
            delete this;
            return;
        }

        if (context->is_completed()) {
            if (!context->try_parse_request_content()) {
                delete this;
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
        delete this;
    }

    bool HttpClient::connect(const InetAddress &addr, connected_callback ccb, request_function rcb)
    {
        if (!ccb || !rcb) {
            std::cout << "must set callback!!\n";
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
        timer::WheelTimerManager manager;
        conn_timer_ = manager.timeout(config::connection_idle_timeout, this);

        net::EventLoop loop(&singleton::Singleton<net::EpollPoller>(), &manager);

        conn->set_connection_handler(this);
        loop.update_event(conn->get_channel());
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
        delete this;
    }
}