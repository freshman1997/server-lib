#ifndef __HTTP_SERVER_H__
#define __HTTP_SERVER_H__
#include <set>
#include <string>
#include <unordered_map>

#include "net/base/handler/connection_handler.h"
#include "common.h"
#include "net/http/request_context.h"
#include "request_dispatcher.h"

namespace net
{
    class Socket;
    class EventLoop;
    class Acceptor;
}

namespace net::http 
{
    class HttpSession;

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
        void free_session(Connection *conn);

        void load_static_paths();

        void serve_static(HttpRequest *req, HttpResponse *resp);

    private:
        bool quit_;
        State state_;
        Acceptor *acceptor_;
        EventLoop *event_loop_;
        std::unordered_map<uint64_t, HttpSession *> sessions_;
        HttpRequestDispatcher dispatcher_;
        std::unordered_map<std::string, std::string> static_paths_;
        std::set<std::string> play_types_;
    };
}

#endif
