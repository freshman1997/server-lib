#include "client/ftp_client.h"
#include "client/client_session.h"
#include "net/connection/connection.h"
#include "net/connection/connection_factory.h"
#include "net/poller/select_poller.h"
#include "net/socket/socket.h"
#include "timer/wheel_timer_manager.h"

#include "logger.h"

#include <chrono>

namespace yuan::net::ftp
{
    FtpClient::FtpClient() : timer_manager_(nullptr), ev_loop_(nullptr), session_(nullptr),
                            owned_timer_manager_(nullptr), owned_ev_loop_(nullptr) {}

    FtpClient::~FtpClient()
    {
        LOG_INFO("ftp client exiting");
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
        notify_connected_state(false);
        LOG_INFO("FTP session closed by server or connection lost");
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

    void FtpClient::notify_connected_state(bool connected)
    {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            connected_ = connected;
            if (!connected) {
                pending_action_ = ClientPendingAction::none;
                transfer_stage_ = ClientTransferStage::idle;
                last_list_output_.clear();
            }
        }
        state_cv_.notify_all();
    }

    void FtpClient::notify_client_context(const ClientContext &context)
    {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            pending_action_ = context.pending_action_;
            transfer_stage_ = context.transfer_stage_;
            last_list_output_ = context.list_output_;
            if (!context.responses_.empty()) {
                last_response_code_ = context.responses_.back().code_;
            }
        }
        state_cv_.notify_all();
    }

    bool FtpClient::wait_until_connected(uint32_t timeout_ms) const
    {
        std::unique_lock<std::mutex> lock(state_mutex_);
        return state_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this]() {
            return connected_;
        });
    }

    bool FtpClient::wait_for_response_code(int code, uint32_t timeout_ms) const
    {
        std::unique_lock<std::mutex> lock(state_mutex_);
        return state_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this, code]() {
            return last_response_code_ == code;
        });
    }

    bool FtpClient::wait_for_transfer_idle(uint32_t timeout_ms) const
    {
        std::unique_lock<std::mutex> lock(state_mutex_);
        return state_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this]() {
            return pending_action_ == ClientPendingAction::none
                && transfer_stage_ == ClientTransferStage::idle;
        });
    }

    bool FtpClient::wait_for_list_output(std::string *output, uint32_t timeout_ms) const
    {
        std::unique_lock<std::mutex> lock(state_mutex_);
        const bool ready = state_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this]() {
            return !last_list_output_.empty();
        });
        if (ready && output) {
            *output = last_list_output_;
        }
        return ready;
    }

    bool FtpClient::wait_for_local_file(const std::filesystem::path &path, uint32_t timeout_ms) const
    {
        std::unique_lock<std::mutex> lock(state_mutex_);
        return state_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this, &path]() {
            return pending_action_ == ClientPendingAction::none
                && transfer_stage_ == ClientTransferStage::idle
                && std::filesystem::exists(path);
        });
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
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            connected_ = false;
            last_response_code_ = 0;
            pending_action_ = ClientPendingAction::none;
            transfer_stage_ = ClientTransferStage::idle;
            last_list_output_.clear();
        }
        state_cv_.notify_all();

        addr_.set_addr(ip, port);
        net::Socket *sock = new net::Socket(addr_.get_ip().c_str(), addr_.get_port());
        if (!sock->valid()) {
            LOG_ERROR("create socket fail!");
            return false;
        }
        sock->set_none_block(true);
        if (!sock->connect()) {
            LOG_WARN("connect failed");
            delete sock;
            return false;
        }

        // Allocate on heap to ensure objects remain alive after connect() returns
        owned_timer_manager_ = new timer::WheelTimerManager();
        owned_ev_loop_ = new net::EventLoop(new SelectPoller(), owned_timer_manager_);
        timer_manager_ = owned_timer_manager_;
        ev_loop_ = owned_ev_loop_;

        auto *conn = create_stream_connection(sock);
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
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            last_response_code_ = 0;
        }
        auto *session = dynamic_cast<ClientFtpSession *>(session_);
        return session ? session->login(username, password) : false;
    }
    bool FtpClient::list(const std::string &path)
    {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            last_list_output_.clear();
        }
        auto *session = dynamic_cast<ClientFtpSession *>(session_);
        return session ? session->list(path) : false;
    }
    bool FtpClient::nlist(const std::string &path)
    {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            last_list_output_.clear();
        }
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
