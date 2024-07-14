#ifndef __NET_FTP_FTP_CLIENT_H__
#define __NET_FTP_FTP_CLIENT_H__
#include "../handler/ftp_app.h"
#include "../common/session.h"
#include "../../../timer/timer_manager.h"
#include "../../base/event/event_loop.h"
#include "../../base/socket/inet_address.h"

namespace net::ftp 
{
    class FtpClient : public FtpApp
    {
    public:
        FtpClient();
        ~FtpClient();

    public: // app
        virtual bool is_ok();

        virtual timer::TimerManager * get_timer_manager();

        virtual EventLoop * get_event_loop();

        virtual void on_session_closed(FtpSession *session);

        virtual void quit();

    public:
        bool connect(const std::string &ip, short port);

        FtpSession * get_session();

        void add_command(const std::string &cmd);

    private:
        InetAddress addr_;
        timer::TimerManager *timer_manager_;
        EventLoop *ev_loop_;
        FtpSession *session_;
    };
}

#endif