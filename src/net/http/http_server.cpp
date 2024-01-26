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

namespace net::http
{
    HttpServer::HttpServer()
    {
        
    }

    HttpServer::~HttpServer()
    {

    }

    void HttpServer::on_connected(Connection *conn)
    {

    }

    void HttpServer::on_error(Connection *conn)
    {

    }

    void HttpServer::on_read(Connection *conn)
    {
        auto context = std::make_shared<HttpRequestContext>(conn);
        if (!context->parse()) {
            std::string msg = "<h1>Internal Server Error</h1>";
            std::string repsonse = "HTTP/1.1 500\r\nContent-Type: text/html; charset=UTF-8\r\nConnection: close\r\nContent-Length: " + std::to_string(msg.size()) + "\r\n\r\n" + msg;
            auto buff = conn->get_output_buff();
            buff->write_string(repsonse);
            conn->send(buff);
            conn->close();
            return;
        }

        auto handler = dispatcher_.get_handler(context->get_request()->get_raw_url());
        if (handler) {
            handler(context);
        } else {
            // 404
            std::string msg = "<h1>resource not found</h1>";
            std::string repsonse = "HTTP/1.1 404 NOT FOUND\r\nContent-Type: text/html; charset=UTF-8\r\nConnection: close\r\nContent-Length: " + std::to_string(msg.size()) + "\r\n\r\n" + msg;
            auto buff = conn->get_output_buff();
            buff->write_string(repsonse);
            conn->send(buff);
            conn->close();
        }
    }

    void HttpServer::on_wirte(Connection *conn)
    {

    }

    void HttpServer::on_close(Connection *conn)
    {
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
        if (!sock->bind()) {
            std::cout << " bind failed " << std::endl;
            return false;
        }

        sock->set_none_block(true);
        acceptor_ = new TcpAcceptor(sock);
        if (!acceptor_->listen()) {
            std::cout << " listen failed " << std::endl;
            return false;
        }

        return true;
    }

    void HttpServer::serve()
    {
        net::PollPoller poller;
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
        dispatcher_.register_handler(url, func);
    }
}