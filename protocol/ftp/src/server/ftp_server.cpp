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

        // Allocate on heap to ensure objects remain alive
        auto *timerManager = new timer::WheelTimerManager();
        auto *poller = new SelectPoller();
        auto *evLoop = new EventLoop(poller, timerManager);

        impl_->ev_loop_ = evLoop;
        impl_->timer_manager_ = timerManager;
        acceptor->set_event_handler(impl_->ev_loop_);
        acceptor->set_connection_handler(this);

        impl_->ev_loop_->loop();

        // Cleanup after event loop exits.
        // If quit() was called while the loop was running, the deferred
        // callbacks already ran and cleaned up sessions/connections.
        // Otherwise, we must clean up manually to prevent leaks.
        {
            auto sessions = impl_->session_manager_.get_sessions();
            for (auto item : sessions) {
                auto *conn = item.second->get_connection();
                if (conn) {
                    conn->set_connection_handler(nullptr);
                    conn->close();
                }
            }
        }
        impl_->session_manager_.clear();
        impl_->ev_loop_ = nullptr;
        impl_->timer_manager_ = nullptr;
        acceptor->close();
        delete acceptor;
        delete evLoop;
        delete poller;
        delete timerManager;

        return true;
    }

    void FtpServer::on_connected(Connection *conn)
    {
        auto session = impl_->session_manager_.get_session(conn);
        if (session) {
            std::cout << "internal error occured!!!\n";
            conn->close();
        } else {
            auto newSessoion = std::make_shared<ServerFtpSession>(conn, this);
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
        // Use remove_by_session because conn_ may already be deleted
        // when on_session_closed is called from a deferred cleanup callback
        impl_->session_manager_.remove_by_session(session);
    }

    void FtpServer::quit()
    {
        if (impl_->closing_) {
            return;
        }

        impl_->closing_ = true;

        // Notify sessions to quit (this sets close_=true and closes connections).
        // After this, on_close() -> delete this or deferred on_session_closed
        // may destroy the sessions. Set closing_ first so on_session_closed
        // skips the remove_by_session call (which would be a no-op anyway
        // since we clear() below).
        auto sessions = impl_->session_manager_.get_sessions();
        for (auto item : sessions) {
            item.second->quit();
        }

        // Clear the session manager: this releases all shared_ptrs.
        // Sessions that called quit() above will have already been deleted
        // via the close_=true path (on_close -> delete this) since quit()
        // calls conn->close() which triggers ~TcpConnection -> on_close.
        // For sessions still in the deferred-cleanup path (close_=false),
        // they are destroyed here when the last shared_ptr is released.
        impl_->session_manager_.clear();
        impl_->timer_manager_ = nullptr;

        if (impl_->ev_loop_) {
            impl_->ev_loop_->quit();
            impl_->ev_loop_ = nullptr;
        }
    }
}