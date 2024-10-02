#ifndef __NET_FTP_FTP_SERVER_H__
#define __NET_FTP_FTP_SERVER_H__
#include <memory>

#include "net/handler/connection_handler.h"
#include "handler/ftp_app.h"
#include "session_manager.h"

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

        virtual EventHandler * get_event_handler();

        virtual void on_session_closed(FtpSession *session);

        virtual void quit();

    private:
        class PrivateImpl;
        std::unique_ptr<PrivateImpl> impl_;
    };
}

#endif