#ifndef __NET_FTP_FTP_SERVER_H__
#define __NET_FTP_FTP_SERVER_H__
#include "net/base/handler/connection_handler.h"
#include <unordered_map>

namespace net 
{
    class EventLoop;
}

namespace timer 
{
    class TimerManager;
}

namespace net::ftp
{ 
    class FtpSession;

    class FtpServer : public ConnectionHandler
    {
    public:
        FtpServer();
        ~FtpServer();

        bool serve(int port);

    public:
        virtual void on_connected(Connection *conn);

        virtual void on_error(Connection *conn);

        virtual void on_read(Connection *conn);

        virtual void on_write(Connection *conn);

        virtual void on_close(Connection *conn);

    public:
        EventLoop * get_even_loop()
        {
            return ev_loop_;
        }

        timer::TimerManager * get_timer_manager()
        {
            return timer_manager_;
        }

    private:
        void on_free_session(FtpSession *session);

    private:
        EventLoop *ev_loop_;
        timer::TimerManager *timer_manager_;
        std::unordered_map<Connection *, FtpSession *> sessions_;
    };
}

#endif