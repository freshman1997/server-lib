#include "net/ftp/server/ftp_server.h"
#include "net/base/acceptor/acceptor.h"
#include "net/base/acceptor/tcp_acceptor.h"
#include "net/base/connection/connection.h"
#include "net/base/event/event_loop.h"
#include "net/base/handler/event_handler.h"
#include "net/base/poller/select_poller.h"
#include "net/base/socket/socket.h"
#include "net/ftp/server/server_session.h"
#include "timer/wheel_timer_manager.h"

#include <iostream>

namespace net::ftp 
{
    FtpServer::FtpServer() : closing_(false)
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

        ev_loop_ = &evLoop;
        timer_manager_ = &timerManager;
        acceptor->set_event_handler(ev_loop_);
        acceptor->set_connection_handler(this);

        ev_loop_->loop();

        delete acceptor;

        return true;
    }

    void FtpServer::on_connected(Connection *conn)
    {
        auto session = session_manager_.get_session(conn);
        if (session) {
            std::cout << "internal error occured!!!\n";
            conn->close();
        } else {
            auto newSessoion = new ServerFtpSession(conn, this);
            session_manager_.add_session(conn, newSessoion);
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
        return ev_loop_ != nullptr;
    }

    timer::TimerManager * FtpServer::get_timer_manager()
    {
        return timer_manager_;
    }

    EventHandler * FtpServer::get_event_handler()
    {
        return ev_loop_;
    }

    void FtpServer::on_session_closed(FtpSession *session)
    {
        if (!session || closing_) {
            return;
        }
        session_manager_.remove_session(session->get_connection());
    }

    void FtpServer::quit()
    {
        if (closing_) {
            return;
        }

        closing_ = true;
        auto sessions = session_manager_.get_sessions();
        for (auto item : sessions) {
            item.second->quit();
        }

        session_manager_.clear();
        timer_manager_ = nullptr;

        if (ev_loop_) {
            ev_loop_->quit();
            ev_loop_ = nullptr;
        }
    }
}