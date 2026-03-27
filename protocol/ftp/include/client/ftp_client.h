#ifndef __NET_FTP_FTP_CLIENT_H__
#define __NET_FTP_FTP_CLIENT_H__
#include "client/context.h"
#include "handler/ftp_app.h"
#include "common/session.h"
#include "timer/timer_manager.h"
#include "event/event_loop.h"
#include "net/socket/inet_address.h"

namespace yuan::net::ftp
{
    class FtpClient : public FtpApp
    {
    public:
        FtpClient();
        ~FtpClient();

        virtual bool is_ok();
        virtual timer::TimerManager * get_timer_manager();
        virtual EventHandler * get_event_handler();
        virtual void on_session_closed(FtpSession *session);
        virtual void quit();

        bool connect(const std::string &ip, short port);
        bool send_command(const std::string &cmd);
        bool login(const std::string &username, const std::string &password);
        bool list(const std::string &path = "");
        bool nlist(const std::string &path = "");
        bool download(const std::string &remote_path, const std::string &local_path);
        bool upload(const std::string &local_path, const std::string &remote_path);
        bool append(const std::string &local_path, const std::string &remote_path);
        const ClientContext * get_client_context() const;
        bool is_transfer_in_progress() const;

    private:
        InetAddress addr_;
        timer::TimerManager *timer_manager_;
        EventLoop *ev_loop_;
        FtpSession *session_;

        // Owned objects to prevent them from being destroyed while in use
        timer::TimerManager *owned_timer_manager_;
        EventLoop *owned_ev_loop_;
    };
}

#endif
