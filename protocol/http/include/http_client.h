#ifndef __NET_HTTP_CLIETNT_H__
#define __NET_HTTP_CLIETNT_H__
#include "event/event_loop.h"
#include "net/handler/connection_handler.h"
#include "net/secuity/ssl_module.h"
#include "net/socket/inet_address.h"
#include "timer/timer.h"
#include "timer/timer_task.h"
#include "common.h"

#include <functional>
#include <memory>

namespace yuan::net::http 
{
    typedef std::function<void (HttpRequest *req)> connected_callback;

    class HttpClient : public ConnectionHandler, public timer::TimerTask
    {
    public:
        HttpClient();
        ~HttpClient();

    public:
        virtual void on_connected(Connection *conn);

        virtual void on_error(Connection *conn);

        virtual void on_read(Connection *conn);

        virtual void on_write(Connection *conn);

        virtual void on_close(Connection *conn);

    public:
        bool query(const std::string &url);

        bool connect(connected_callback ccb, request_function rcb);
    
        virtual void on_timer(timer::Timer *timer);

        virtual void on_finished(timer::Timer *timer)
        {
            delete this;
        }

    private:
        void exit();

    private:
        int port_;
        std::string host_name_;
        net::EventLoop *ev_loop_;
        HttpSession *session_;
        request_function rcb_;
        connected_callback ccb_;
        timer::TimerManager *timer_manager_;
        timer::Timer *conn_timer_;
        std::shared_ptr<SSLModule> ssl_module_;
    };
}

#endif