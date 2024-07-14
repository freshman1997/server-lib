#ifndef __NET_FTP_FTP_SERVER_H__
#define __NET_FTP_FTP_SERVER_H__
#include "../../base/handler/connection_handler.h"
#include "../common/session_manager.h"
#include "../handler/ftp_app.h"

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

    class FtpServer : public ConnectionHandler, public FtpApp
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

    public: // app
        virtual bool is_ok();

        virtual timer::TimerManager * get_timer_manager();

        virtual EventLoop * get_event_loop();

        virtual void on_session_closed(FtpSession *session);

        virtual void quit();

    private:
        bool closing_;
        EventLoop *ev_loop_;
        timer::TimerManager *timer_manager_;
        FtpSessionManager session_manager_;
    };
}

#endif