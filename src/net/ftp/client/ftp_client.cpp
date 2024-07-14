#include "net/ftp/client/ftp_client.h"
#include "net/base/acceptor/tcp_acceptor.h"
#include "net/base/connection/tcp_connection.h"
#include "net/base/poller/select_poller.h"
#include "net/base/socket/socket.h"
#include "net/ftp/common/session.h"
#include "singleton/singleton.h"
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

    EventLoop * FtpClient::get_event_loop()
    {
        return ev_loop_;
    }

    void FtpClient::on_session_closed(FtpSession *session)
    {
        // do nothing
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

        TcpConnection *conn = new TcpConnection(sock);
        timer::WheelTimerManager manager;

        net::EventLoop loop(&singleton::Singleton<net::SelectPoller>(), &manager);
        timer_manager_ = &manager;
        ev_loop_ = &loop;

        session_ = new FtpSession(conn, this, FtpSessionWorkMode::client);
        loop.update_event(conn->get_channel());
        conn->set_event_handler(&loop);
        loop.loop();
        
        return true;
    }

    FtpSession * FtpClient::get_session()
    {
        return session_;
    }

    void FtpClient::add_command(const std::string &cmd)
    {
        session_->add_command(cmd);
    }
}