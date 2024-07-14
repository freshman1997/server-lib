#include "net/ftp/server/ftp_server.h"
#include "net/base/acceptor/acceptor.h"
#include "net/base/acceptor/tcp_acceptor.h"
#include "net/base/connection/connection.h"
#include "net/base/event/event_loop.h"
#include "net/base/poller/select_poller.h"
#include "net/base/socket/socket.h"
#include "net/ftp/common/session.h"
#include "timer/wheel_timer_manager.h"

#include <iostream>

namespace net::ftp 
{
    FtpServer::FtpServer()
    {

    }

    FtpServer::~FtpServer()
    {
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

        return true;
    }

    void FtpServer::on_connected(Connection *conn)
    {
        auto session = session_manager_.get_session(conn);
        if (session) {
            std::cout << "internal error occured!!!\n";
            conn->close();
            return;
        }
        auto newSession = new FtpSession(conn, this);
        session_manager_.add_session(conn, newSession);
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

    EventLoop * FtpServer::get_event_loop()
    {
        return ev_loop_;
    }

    void FtpServer::on_session_closed(FtpSession *session)
    {
        if (!session) {
            return;
        }
        ev_loop_->on_close(session->get_connection());
        session_manager_.remove_session(session->get_connection());
        session->quit();
    }

    void FtpServer::quit()
    {
        auto sessions = session_manager_.get_sessions();
        for (auto item : sessions) {
            item.second->quit();
        }
        
        timer_manager_ = nullptr;

        if (ev_loop_) {
            ev_loop_->quit();
            ev_loop_ = nullptr;
        }
    }
}