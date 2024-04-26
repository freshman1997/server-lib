#include "net/ftp/ftp_server.h"
#include "net/base/acceptor/acceptor.h"
#include "net/base/acceptor/tcp_acceptor.h"
#include "net/base/connection/connection.h"
#include "net/base/event/event_loop.h"
#include "net/base/poller/epoll_poller.h"
#include "net/base/poller/select_poller.h"
#include "net/base/socket/socket.h"
#include "net/ftp/session.h"
#include "timer/wheel_timer_manager.h"

#include <iostream>

namespace net::ftp 
{
    FtpServer::FtpServer()
    {

    }

    FtpServer::~FtpServer()
    {
        for (auto &it : sessions_) {
            on_free_session(it.second);
            delete it.second;
        }
        sessions_.clear();
    }

    bool FtpServer::serve(int port)
    {
        Socket *sock = new Socket("127.0.0.1", port);
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
        ev_loop_->set_connection_handler(this);

        ev_loop_->loop();

        return true;
    }

    void FtpServer::on_connected(Connection *conn)
    {
        auto it = sessions_.find(conn);
        if (it != sessions_.end()) {
            std::cout << "internal error occured!!!\n";
            conn->close();
            return;
        }
        sessions_[conn] = new FtpSession(conn);
    }

    void FtpServer::on_error(Connection *conn)
    {
        
    }

    void FtpServer::on_read(Connection *conn)
    {
        auto it = sessions_.find(conn);
        if (it != sessions_.end()) {
            std::cout << "internal error occured!!!\n";
            conn->close();
            return;
        }
        it->second->on_packet();
    }

    void FtpServer::on_write(Connection *conn)
    {

    }

    void FtpServer::on_close(Connection *conn)
    {
        auto it = sessions_.find(conn);
        if (it == sessions_.end()) {
            std::cout << "internal error occured!!!\n";
            return;
        }

        on_free_session(it->second);
        delete it->second;
        sessions_.erase(it);
    }

    void FtpServer::on_free_session(FtpSession *session)
    {
        if (!session) {
            return;
        }

        ev_loop_->on_close(session->get_connection());
    }
}