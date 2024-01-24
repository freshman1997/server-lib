#ifndef __HTTP_SERVER_H__
#define __HTTP_SERVER_H__
#include <fstream>
#include <functional>
#include <memory>

#include "net/handler/connection_handler.h"
#include "common.h"
#include "ops/request_dispatcher.h"
#include "request.h"

namespace net
{
    class Socket;
    class EventLoop;
    class Acceptor;
}

namespace net::http 
{
    class HttpServer : public ConnectionHandler
    {
        enum State
        {
            invalid = -1,
            create_socket,
            bind_address,
            listen_address,
            create_acceptor,
            create_poller,
            create_event_loop,
            success
        };        

    public:
        HttpServer();
        ~HttpServer();

    public:
        virtual void on_connected(Connection *conn);

        virtual void on_error(Connection *conn);

        virtual void on_read(Connection *conn);

        virtual void on_wirte(Connection *conn);

        virtual void on_close(Connection *conn);

    public:
        bool init(int port);

        void serve();

        void stop();

    public:
        void on(const std::string &url, request_function func);

    private:
        bool quit_;
        State state_;
        Acceptor *acceptor_;
        EventLoop *event_loop_;
        
        HttpRequestDispatcher dispatcher_;
    };
}

#endif
