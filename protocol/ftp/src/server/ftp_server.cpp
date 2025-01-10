#include "server/ftp_server.h"
#include "net/acceptor/acceptor.h"
#include "net/acceptor/tcp_acceptor.h"
#include "net/connection/connection.h"
#include "event/event_loop.h"
#include "net/handler/event_handler.h"
#include "net/poller/select_poller.h"
#include "net/socket/socket.h"
#include "server/server_session.h"
#include "timer/wheel_timer_manager.h"

#include <iostream>
#include <memory>

namespace yuan::net::ftp 
{
    class FtpServer::PrivateImpl
    {
    public:
        explicit PrivateImpl()
        {
            closing_ = false;
            ev_loop_ = nullptr;
            timer_manager_ = nullptr;
        }

        PrivateImpl(const PrivateImpl &) = delete;
        PrivateImpl & operator=(const PrivateImpl &) = delete;

        bool closing_;
        EventLoop *ev_loop_;
        timer::TimerManager *timer_manager_;
        FtpSessionManager session_manager_;
    };

    FtpServer::FtpServer() : impl_(std::make_unique<PrivateImpl>())
    {

    }

    FtpServer::~FtpServer()
    {
        std::cout << "ftp server exit now!\n";
        quit();
    }

    bool FtpServer::serve(int port)
    {
        Socket *sock = new Socket("", port);
        if (!sock->valid()) {
            std::cout << "cant create socket file descriptor!\n";
            delete sock;
            return false;
        }

        if (!sock->bind()) {
            std::cout << "cant bind port: " << port << "!\n";
            delete sock;
            return false;
        }

        sock->set_reuse(true);
        sock->set_none_block(true);

        TcpAcceptor *acceptor = new TcpAcceptor(sock);
        if (!acceptor->listen()) {
            std::cout << "cant listen on port: " << port << "!\n";
            delete acceptor;
            return false;
        }

        timer::WheelTimerManager timerManager;
        SelectPoller poller;
        EventLoop evLoop(&poller, &timerManager);

        impl_->ev_loop_ = &evLoop;
        impl_->timer_manager_ = &timerManager;
        acceptor->set_event_handler(impl_->ev_loop_);
        acceptor->set_connection_handler(this);

        impl_->ev_loop_->loop();

        acceptor->close();

        return true;
    }

    void FtpServer::on_connected(Connection *conn)
    {
        auto session = impl_->session_manager_.get_session(conn);
        if (session) {
            std::cout << "internal error occured!!!\n";
            conn->close();
        } else {
            auto newSessoion = new ServerFtpSession(conn, this);
            impl_->session_manager_.add_session(conn, newSessoion);
            newSessoion->on_connected(conn);
        }
    }

    void FtpServer::on_error(Connection *conn)
    {

    }

    void FtpServer::on_read(Connection *conn)
    {
        std::cerr << "on_read internal error occured!!!\n";
    }

    void FtpServer::on_write(Connection *conn)
    {
        std::cerr << "on_write internal error occured!!!\n";
    }

    void FtpServer::on_close(Connection *conn)
    {

    }

    bool FtpServer::is_ok()
    {
        return impl_ && impl_->ev_loop_ != nullptr;
    }

    timer::TimerManager * FtpServer::get_timer_manager()
    {
        return impl_->timer_manager_;
    }

    EventHandler * FtpServer::get_event_handler()
    {
        return impl_->ev_loop_;
    }

    void FtpServer::on_session_closed(FtpSession *session)
    {
        if (!session || impl_->closing_) {
            return;
        }
        impl_->session_manager_.remove_session(session->get_connection());
    }

    void FtpServer::quit()
    {
        if (impl_->closing_) {
            return;
        }

        impl_->closing_ = true;
        auto sessions = impl_->session_manager_.get_sessions();
        for (auto item : sessions) {
            item.second->quit();
        }

        impl_->session_manager_.clear();
        impl_->timer_manager_ = nullptr;

        if (impl_->ev_loop_) {
            impl_->ev_loop_->quit();
            impl_->ev_loop_ = nullptr;
        }
    }
}