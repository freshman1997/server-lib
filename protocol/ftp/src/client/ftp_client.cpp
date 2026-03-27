#include "client/ftp_client.h"
#include "client/client_session.h"
#include "net/connection/connection.h"
#include "net/connection/tcp_connection.h"
#include "net/poller/select_poller.h"
#include "net/socket/socket.h"
#include "timer/wheel_timer_manager.h"

#include <iostream>

namespace yuan::net::ftp
{
    FtpClient::FtpClient() : timer_manager_(nullptr), ev_loop_(nullptr), session_(nullptr),
                            owned_timer_manager_(nullptr), owned_ev_loop_(nullptr) {}

    FtpClient::~FtpClient()
    {
        std::cout << "ftp client exit now!\n";
        if (owned_ev_loop_) {
            delete owned_ev_loop_;
            owned_ev_loop_ = nullptr;
        }
        if (owned_timer_manager_) {
            delete owned_timer_manager_;
            owned_timer_manager_ = nullptr;
        }
    }

    bool FtpClient::is_ok() { return ev_loop_ != nullptr && session_ != nullptr && session_->get_connection() != nullptr && session_->get_connection()->is_connected(); }
    timer::TimerManager *FtpClient::get_timer_manager() { return timer_manager_; }
    EventHandler *FtpClient::get_event_handler() { return ev_loop_; }

    void FtpClient::on_session_closed(FtpSession *session)
    {
        // Delete the session: on the client side, the session is owned via raw pointer.
        // On the server side, deletion is handled by shared_ptr in session_manager.
        delete session;
        session_ = nullptr;
        std::cout << "FTP session closed by server or connection lost" << std::endl;
        // Quit the event loop so connect() returns and the thread finishes.
        // This allows the user to reconnect from the same process.
        if (ev_loop_) {
            ev_loop_->quit();
        }
    }

    void FtpClient::quit()
    {
        if (session_) {
            // session_->quit() defers on_session_closed via queue_in_loop,
            // which calls ev_loop_->quit() from within the event loop thread.
            // Do NOT call ev_loop_->quit() here — it races with the event loop:
            // if the loop is in cond.wait(), it wakes up and exits BEFORE
            // processing the deferred callback, leaking the session/connection.
            session_->quit();
        }
        // If session_ is null, on_session_closed already ran and quit the loop.
    }

    bool FtpClient::connect(const std::string &ip, short port)
    {
        // Clean up previous connection resources if reconnecting
        if (session_) {
            Connection *oldConn = session_->get_connection();
            session_->quit();
            if (oldConn) {
                oldConn->set_connection_handler(nullptr);
                oldConn->close();
            }
            delete session_;
            session_ = nullptr;
        }
        if (owned_ev_loop_) {
            delete owned_ev_loop_;
            owned_ev_loop_ = nullptr;
        }
        if (owned_timer_manager_) {
            delete owned_timer_manager_;
            owned_timer_manager_ = nullptr;
        }
        timer_manager_ = nullptr;
        ev_loop_ = nullptr;

        addr_.set_addr(ip, port);
        net::Socket *sock = new net::Socket(addr_.get_ip().c_str(), addr_.get_port());
        if (!sock->valid()) {
            std::cout << "create socket fail!!\n";
            return false;
        }
        sock->set_none_block(true);
        if (!sock->connect()) {
            std::cout << " connect failed \n";
            delete sock;
            return false;
        }

        // Allocate on heap to ensure objects remain alive after connect() returns
        owned_timer_manager_ = new timer::WheelTimerManager();
        owned_ev_loop_ = new net::EventLoop(new SelectPoller(), owned_timer_manager_);
        timer_manager_ = owned_timer_manager_;
        ev_loop_ = owned_ev_loop_;

        auto *conn = new TcpConnection(sock);
        session_ = new ClientFtpSession(conn, this);
        conn->set_event_handler(ev_loop_);
        ev_loop_->loop();

        // Cleanup after loop exits.
        // If the deferred callback from session_->quit() ran, session_ is null
        // and everything is already cleaned up. Otherwise, clean up manually.
        if (session_) {
            // Save connection before quit() clears it from context
            Connection *conn = session_->get_connection();
            session_->quit();
            // Close the connection manually (deferred callback didn't run)
            if (conn) {
                conn->set_connection_handler(nullptr);
                conn->close();
            }
            delete session_;
            session_ = nullptr;
        }

        return true;
    }
    bool FtpClient::send_command(const std::string &cmd) { return session_ ? session_->send_command(cmd) : false; }
    bool FtpClient::login(const std::string &username, const std::string &password)
    {
        auto *session = dynamic_cast<ClientFtpSession *>(session_);
        return session ? session->login(username, password) : false;
    }
    bool FtpClient::list(const std::string &path)
    {
        auto *session = dynamic_cast<ClientFtpSession *>(session_);
        return session ? session->list(path) : false;
    }
    bool FtpClient::nlist(const std::string &path)
    {
        auto *session = dynamic_cast<ClientFtpSession *>(session_);
        return session ? session->nlist(path) : false;
    }
    bool FtpClient::download(const std::string &remote_path, const std::string &local_path)
    {
        auto *session = dynamic_cast<ClientFtpSession *>(session_);
        return session ? session->download(remote_path, local_path) : false;
    }
    bool FtpClient::upload(const std::string &local_path, const std::string &remote_path)
    {
        auto *session = dynamic_cast<ClientFtpSession *>(session_);
        return session ? session->upload(local_path, remote_path) : false;
    }
    bool FtpClient::append(const std::string &local_path, const std::string &remote_path)
    {
        auto *session = dynamic_cast<ClientFtpSession *>(session_);
        return session ? session->append(local_path, remote_path) : false;
    }
    const ClientContext *FtpClient::get_client_context() const
    {
        auto *session = dynamic_cast<ClientFtpSession *>(session_);
        return session ? &session->get_client_context() : nullptr;
    }
    bool FtpClient::is_transfer_in_progress() const
    {
        auto *session = dynamic_cast<ClientFtpSession *>(session_);
        if (!session) {
            return false;
        }
        const auto *ctx = get_client_context();
        return ctx && ctx->pending_action_ != ClientPendingAction::none && ctx->transfer_stage_ != ClientTransferStage::idle;
    }
}
