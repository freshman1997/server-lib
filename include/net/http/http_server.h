#ifndef __HTTP_SERVER_H__
#define __HTTP_SERVER_H__
#include <fstream>

#include "net/handler/tcp_socket_handler.h"

namespace net
{
    class Socket;
    class EventLoop;
    class Acceptor;
}

namespace net::http 
{
    class HttpServer : public TcpConnectionHandler
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
        virtual void on_connected(TcpConnection *conn);

        virtual void on_error(TcpConnection *conn);

        virtual void on_read(TcpConnection *conn);

        virtual void on_wirte(TcpConnection *conn);

        virtual void on_close(TcpConnection *conn);

    public:
        bool init(int port);

        void serve();

        void stop();

    private:
        bool quit_;
        State state_;
        Acceptor *acceptor_;
        EventLoop *event_loop_;
        char *buff1_;
        std::fstream file_;
        long long length_;
        //net::http::HttpRequest req_;
    };
}

#endif
