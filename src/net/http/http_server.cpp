#include <iostream>
#include <memory>
#include <unistd.h>

#ifdef _WIN32
#include <windows.h>
#include<WinSock2.h>
#endif

#include "buffer/buffer.h"
#include "net/acceptor/tcp_acceptor.h"
#include "net/event/event_loop.h"
#include "net/http/http_server.h"
#include "net/http/request.h"
#include "net/poller/epoll_poller.h"
#include "net/poller/poll_poller.h"
#include "net/poller/select_poller.h"
#include "net/socket/socket.h"
#include "timer/wheel_timer_manager.h"
#include "net/connection/connection.h"
#include "base/time.h"
#include "net/http/session.h"
#include "net/http/response_code.h"

namespace net::http
{
    HttpServer::HttpServer()
    {
        
    }

    HttpServer::~HttpServer()
    {
        for (const auto &it : sessions_) {
            delete it.second;
        }

        sessions_.clear();
    }

    void HttpServer::on_connected(Connection *conn)
    {
        uint64_t sessionId = (uint64_t)conn;
        sessions_[sessionId] = new HttpSession(sessionId, new HttpRequestContext(conn));
    }

    void HttpServer::on_error(Connection *conn)
    {
        free_session(conn);
    }

    void HttpServer::on_read(Connection *conn)
    {
        auto context = sessions_[(uint64_t)conn]->get_context();
        context->pre_request();

        if (!context->parse()) {
            return;
        }

        if (context->has_error()) {
            context->process_error(conn, context->get_error_code());
            return;
        }

        if (context->is_completed()) {
            auto handler = dispatcher_.get_handler(context->get_request()->get_raw_url());
            if (handler) {
                handler(context->get_request(), context->get_response());
            } else {
                // 404
                context->process_error(conn, ResponseCode::not_found);
                return;
            }
        }
    }

    void HttpServer::on_wirte(Connection *conn)
    {
        
    }

    void HttpServer::on_close(Connection *conn)
    {
        free_session(conn);
        event_loop_->on_close(conn);
    }

    bool HttpServer::init(int port)
    {
        net::Socket *sock = new net::Socket("", port);
        if (!sock->valid()) {
            std::cout << "create socket fail!!\n";
            return false;
        }

        sock->set_reuse(true);
        sock->set_none_block(true);
        if (!sock->bind()) {
            std::cout << " bind failed " << std::endl;
            return false;
        }

        acceptor_ = new TcpAcceptor(sock);
        if (!acceptor_->listen()) {
            std::cout << " listen failed " << std::endl;
            return false;
        }

        return true;
    }

    void HttpServer::serve()
    {
        net::SelectPoller poller;
        timer::WheelTimerManager manager;
        net::EventLoop loop(&poller, &manager);
        acceptor_->set_event_handler(&loop);

        loop.update_event(acceptor_->get_channel());
        this->event_loop_ = &loop;
        loop.set_connection_handler(this);
        loop.loop();
    }

    void HttpServer::stop()
    {
        event_loop_->quit();
    }

    void HttpServer::on(const std::string &url, request_function func)
    {
        if (url.empty() || !func) {
            return;
        }

        dispatcher_.register_handler(url, func);
    }

    void HttpServer::free_session(Connection *conn)
    {
        uint64_t sessionId = (uint64_t)conn;
        auto it = sessions_.find(sessionId);
        if (it != sessions_.end()) {
            delete it->second;
            sessions_.erase(it);
        } else {
            std::cerr << "internal error found!!!\n";
        }
    }
}