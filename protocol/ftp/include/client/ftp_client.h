#ifndef __NET_FTP_FTP_CLIENT_H__
#define __NET_FTP_FTP_CLIENT_H__
#include "client/context.h"
#include "handler/ftp_app.h"
#include "common/session.h"
#include "timer/timer_manager.h"
#include "event/event_loop.h"
#include "net/socket/inet_address.h"

#include <filesystem>
#include <condition_variable>
#include <mutex>

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
        bool wait_until_connected(uint32_t timeout_ms = 10000) const;
        bool wait_for_response_code(int code, uint32_t timeout_ms = 10000) const;
        bool wait_for_transfer_idle(uint32_t timeout_ms = 15000) const;
        bool wait_for_list_output(std::string *output = nullptr, uint32_t timeout_ms = 10000) const;
        bool wait_for_local_file(const std::filesystem::path &path, uint32_t timeout_ms = 15000) const;

        void notify_connected_state(bool connected);
        void notify_client_context(const ClientContext &context);

    private:
        InetAddress addr_;
        timer::TimerManager *timer_manager_;
        EventLoop *ev_loop_;
        FtpSession *session_;

        // Owned objects to prevent them from being destroyed while in use
        timer::TimerManager *owned_timer_manager_;
        EventLoop *owned_ev_loop_;

        mutable std::mutex state_mutex_;
        mutable std::condition_variable state_cv_;
        bool connected_ = false;
        int last_response_code_ = 0;
        ClientPendingAction pending_action_ = ClientPendingAction::none;
        ClientTransferStage transfer_stage_ = ClientTransferStage::idle;
        std::string last_list_output_;
    };
}

#endif
