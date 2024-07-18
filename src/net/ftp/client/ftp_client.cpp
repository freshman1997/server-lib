#include "net/ftp/client/ftp_client.h"
#include "net/base/acceptor/tcp_acceptor.h"
#include "net/base/connection/tcp_connection.h"
#include "net/base/handler/event_handler.h"
#include "net/base/poller/select_poller.h"
#include "net/base/socket/socket.h"
#include "net/ftp/client/client_session.h"
#include "timer/wheel_timer_manager.h"

#include <iostream>

namespace net::ftp 
{
    FtpClient::FtpClient()
    {
        session_ = nullptr;
        timer_manager_ = nullptr;
        ev_loop_ = nullptr;
    }

    FtpClient::~FtpClient()
    { 
        std::cout << "ftp client exit now!\n";
    }

    bool FtpClient::is_ok()
    {
        return ev_loop_ != nullptr 
            && session_ != nullptr 
            && session_->get_connection() != nullptr 
            && session_->get_connection()->is_connected();
    }

    timer::TimerManager * FtpClient::get_timer_manager()
    {
        return timer_manager_;
    }

    EventHandler * FtpClient::get_event_handler()
    {
        return ev_loop_;
    }

    void FtpClient::on_session_closed(FtpSession *session)
    {
        // 被动关闭需要置空 session_ 指针
        session_ = nullptr;
        quit();
    }

    void FtpClient::quit()
    {
        if (session_) {
            session_->quit();
        }

        if (ev_loop_) {
            ev_loop_->quit();
        }
    }

    bool FtpClient::connect(const std::string &ip, short port)
    {
        addr_.set_addr(ip, port);
        net::Socket *sock = new net::Socket(addr_.get_ip().c_str(), addr_.get_port());
        if (!sock->valid()) {
            std::cout << "create socket fail!!\n";
            return false;
        }

        sock->set_none_block(true);
        if (!sock->connect()) {
            std::cout << " connect failed " << std::endl;
            return false;
        }

        timer::WheelTimerManager manager;
        SelectPoller poller;
        net::EventLoop loop(&poller, &manager);
        timer_manager_ = &manager;
        ev_loop_ = &loop;

        TcpConnection *conn = new TcpConnection(sock);
        session_ = new ClientFtpSession(conn, this);
        loop.update_channel(conn->get_channel());
        conn->set_event_handler(&loop);
        loop.loop();
        
        return true;
    }

    bool FtpClient::send_command(const std::string &cmd)
    {
        return session_ ? session_->send_command(cmd) : false;
    }
}