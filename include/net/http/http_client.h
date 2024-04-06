#ifndef __NET_HTTP_CLIETNT_H__
#define __NET_HTTP_CLIETNT_H__
#include "net/base/event/event_loop.h"
#include "net/base/handler/connection_handler.h"
#include "net/base/socket/inet_address.h"
#include "timer/timer.h"
#include "timer/timer_task.h"
#include "net/http/common.h"

#include <functional>

namespace net::http 
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
        bool connect(const InetAddress &addr, connected_callback ccb, request_function rcb);
    
        virtual void on_timer(timer::Timer *timer);

        virtual void on_finished(timer::Timer *timer)
        {

        }

    private:
        net::EventLoop *ev_loop_;
        HttpSession *session_;
        request_function rcb_;
        connected_callback ccb_;
        timer::TimerManager *timer_manager_;
        timer::Timer *conn_timer_;
    };
}

#endif