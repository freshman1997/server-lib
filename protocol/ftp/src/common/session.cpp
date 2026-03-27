#include "common/session.h"
#include "client/client_file_stream.h"
#include "common/def.h"
#include "common/file_stream.h"
#include "common/file_stream_session.h"
#include "handler/ftp_app.h"
#include "server/context.h"
#include "server/server_file_stream.h"

#include <cassert>
#include <filesystem>
#include <iostream>

namespace yuan::net::ftp
{
    namespace
    {
        std::string normalize_dir(const std::string &dir)
        {
            namespace fs = std::filesystem;
            return dir.empty() ? fs::current_path().lexically_normal().generic_string() : fs::path(dir).lexically_normal().generic_string();
        }
    }

    FtpSessionContext::FtpSessionContext() : instance_(nullptr), file_stream_(nullptr), app_(nullptr), conn_(nullptr) {}

    FtpSessionContext::~FtpSessionContext()
    {
        for (auto it : values) {
            if (it.second.item.sval && it.second.type == FtpSessionValueType::string_val) {
                delete it.second.item.sval;
            }
        }
    }

    void FtpSessionContext::close()
    {
        if (file_stream_) {
            file_stream_->quit(conn_ ? conn_->get_remote_address() : InetAddress{});
            file_stream_ = nullptr;
        }
        Connection *conn = conn_;
        conn_ = nullptr;
        passive_addr_.reset();
        if (app_) {
            auto *app = app_;
            app_ = nullptr;
            app->on_session_closed(instance_);
        }
        if (conn) {
            // Clear handler before closing: ~TcpConnection calls
            // connectionHandler_->on_close() if handler is not null,
            // which would re-enter the (now destroying) session.
            conn->set_connection_handler(nullptr);
            conn->close();
        }
    }

    FtpSession::FtpSession(Connection *conn, FtpApp *app, WorkMode mode, bool keepUtilSent) : work_mode_(mode), keep_util_sent_(keepUtilSent), close_(false)
    {
        context_.conn_ = conn;
        context_.app_ = app;
        context_.conn_->set_connection_handler(this);
        context_.instance_ = this;
        context_.root_dir_ = normalize_dir(ServerContext::get_instance()->get_server_work_dir());
        context_.cwd_ = "/";
    }

    FtpSession::~FtpSession()
    {
        // Don't set close_ flag here - it should only be set when explicitly calling quit()
        // This prevents session from being destroyed during data transfer operations
        // Note: if quit() was called and its deferred callback already ran,
        // context_ fields (conn_, app_, file_stream_) are already null,
        // so context_.close() is a no-op.
        context_.close();
        std::cout << "ftp session closed!\n";
    }

    void FtpSession::on_connected(Connection *conn) { (void)conn; }
    void FtpSession::on_error(Connection *conn) { (void)conn; }
    void FtpSession::on_write(Connection *conn) { (void)conn; }

    void FtpSession::on_close(Connection *conn)
    {
        (void)conn;
        if (close_) {
            // Session was explicitly quit() — the deferred callback from quit()
            // already handles on_session_closed and connection close.
            // Since quit()'s deferred callback sets connectionHandler_ to null
            // before calling conn->close(), this on_close() should NOT be
            // reached. If it IS reached (e.g., remote closes before the
            // deferred callback runs), just return — the deferred callback
            // will handle everything.
            return;
        }
        // Connection closed by remote end (abrupt disconnect).
        // Defer cleanup to avoid self-deletion while still on the call stack
        // (~TcpConnection -> on_close -> we are here).
        close_ = true;
        auto *app = context_.app_;
        auto *instance = this;
        auto *file_stream = context_.file_stream_;
        auto *pending_ffs = pending_file_stream_cleanup_;
        // Save remote address before conn_ becomes dangling
        InetAddress remote_addr = context_.conn_ ? context_.conn_->get_remote_address() : InetAddress{};
        // Clear immediately to prevent reentrant access
        context_.app_ = nullptr;
        context_.conn_ = nullptr;
        context_.passive_addr_.reset();
        context_.file_stream_ = nullptr;
        pending_file_stream_cleanup_ = nullptr;
        if (app) {
            app->get_event_handler()->queue_in_loop([app, instance, file_stream, pending_ffs, remote_addr]() {
                if (file_stream) {
                    file_stream->quit(remote_addr);
                }
                if (pending_ffs && pending_ffs != file_stream) {
                    pending_ffs->quit(remote_addr);
                }
                // Notify the app (server removes from session_manager, client quits loop).
                // Note: during server shutdown (FtpServer::quit()), the closing_ flag
                // is set BEFORE sessions are cleared. The on_session_closed callback
                // checks this flag and returns early, so the dangling 'instance'
                // pointer is never dereferenced.
                app->on_session_closed(instance);
            });
        }
    }

    void FtpSession::on_timer(timer::Timer *timer)
    {
        (void)timer;
        if (close_) {
            return;
        }
        // Timer timeout: treat like an abrupt disconnect, use deferred cleanup
        quit();
    }

    void FtpSession::on_opened(FtpFileStreamSession *fs) { (void)fs; }

    void FtpSession::on_connect_timeout(FtpFileStreamSession *fs)
    {
        if (work_mode_ == WorkMode::server) {
            send_command("425 Can't open data connection.\r\n");
        }
        check_file_stream(fs);
        clear_passive_addr();
    }

    void FtpSession::on_start(FtpFileStreamSession *fs) { (void)fs; }

    void FtpSession::on_error(FtpFileStreamSession *fs)
    {
        (void)fs;
        if (work_mode_ == WorkMode::server) {
            send_command("426 Connection closed; transfer aborted.\r\n");
        }
        clear_passive_addr();
    }

    void FtpSession::on_completed(FtpFileStreamSession *fs)
    {
        (void)fs;
        if (work_mode_ == WorkMode::server) {
            send_command("226 Transfer complete.\r\n");
        }
        if (context_.file_manager_.is_completed()) {
            context_.file_manager_.reset();
        }
        // Release the file_stream reference immediately so the next PASV
        // command creates a fresh FtpFileStream/acceptor on a new port.
        // Save the pointer for deferred cleanup via on_closed.
        pending_file_stream_cleanup_ = context_.file_stream_;
        context_.file_stream_ = nullptr;
        clear_passive_addr();
    }

    void FtpSession::on_closed(FtpFileStreamSession *fs)
    {
        if (pending_file_stream_cleanup_ && fs)
        {
            // Deferred cleanup: the data connection has been closed (we're in the
            // on_closed callback from FtpFileStreamSession::quit()), so it's safe
            // to clean up the FtpFileStream now. We defer via queue_in_loop to
            // ensure the current call stack (which still references the session)
            // has fully unwound before the FtpFileStream deletes the session.
            auto *ffs = pending_file_stream_cleanup_;
            pending_file_stream_cleanup_ = nullptr;
            auto *app = context_.app_;
            if (app) {
                app->get_event_handler()->queue_in_loop([ffs, fs]() {
                    ffs->quit(fs->get_remote_address());
                });
            }
        }
        else
        {
            check_file_stream(fs);
        }
    }
    void FtpSession::on_idle_timeout(FtpFileStreamSession *fs) { check_file_stream(fs); clear_passive_addr(); }
    void FtpSession::quit()
    {
        if (close_) {
            return;
        }
        close_ = true;

        // Save state on stack before any operation that might destroy this
        Connection *conn = context_.conn_;
        FtpApp *app = context_.app_;
        FtpFileStream *ffs = context_.file_stream_;
        FtpFileStream *pending_ffs = pending_file_stream_cleanup_;
        InetAddress remote_addr = conn ? conn->get_remote_address() : InetAddress{};

        // Clear context immediately to prevent reentrant access
        context_.conn_ = nullptr;
        context_.app_ = nullptr;
        context_.file_stream_ = nullptr;
        context_.passive_addr_.reset();
        pending_file_stream_cleanup_ = nullptr;

        // Close file stream if exists
        if (ffs) {
            ffs->quit(remote_addr);
        }
        if (pending_ffs && pending_ffs != ffs) {
            pending_ffs->quit(remote_addr);
        }

        // Defer ALL cleanup to pending callbacks section of the event loop.
        // This is critical: if on_session_closed is called directly here, it
        // deletes the session (server: shared_ptr released, client: delete session).
        // But the event loop is still inside channel->on_event() processing.
        // If the connection has both READ and WRITE events, on_write_event()
        // would be called next, accessing the already-deleted session via
        // connectionHandler_->on_write(this) -> use-after-free -> crash.
        // By deferring, we ensure all channel events finish before cleanup.
        if (conn && app) {
            app->get_event_handler()->queue_in_loop([conn, app, this]() {
                // Notify app: server removes shared_ptr (may delete this),
                // client deletes session and quits event loop.
                app->on_session_closed(this);
                // Clear handler before closing to prevent on_close() from
                // accessing the (now deleted) session.
                conn->set_connection_handler(nullptr);
                conn->close();
            });
        } else if (conn) {
            conn->set_connection_handler(nullptr);
            conn->close();
        } else if (app) {
            app->on_session_closed(this);
        }
    }

    bool FtpSession::login()
    {
        if (context_.user_.username_.empty() || context_.user_.password_.empty()) {
            return false;
        }
        context_.user_.logined_ = true;
        return true;
    }

    void FtpSession::set_username(const std::string &username) { context_.user_.username_ = username; context_.user_.logined_ = false; }
    void FtpSession::set_password(const std::string &passwd) { context_.user_.password_ = passwd; context_.user_.logined_ = false; }

    bool FtpSession::start_file_stream(const InetAddress &addr, StreamMode mode)
    {
        (void)mode;
        assert(context_.app_);
        if (context_.file_stream_) {
            return true;
        }
        if (work_mode_ == WorkMode::server) {
            context_.file_stream_ = new ServerFtpFileStream(this);
        } else {
            context_.file_stream_ = new ClientFtpFileStream(this);
        }
        if (!context_.file_stream_->start(addr)) {
            delete context_.file_stream_;
            context_.file_stream_ = nullptr;
            return false;
        }
        return true;
    }

    void FtpSession::set_passive_addr(const InetAddress &addr) { context_.passive_addr_ = addr; }
    void FtpSession::clear_passive_addr() { context_.passive_addr_.reset(); }

    void FtpSession::check_file_stream(FtpFileStreamSession *fs)
    {
        if (context_.file_stream_ && fs) {
            context_.file_stream_->quit(fs->get_remote_address());
        }
    }

    bool FtpSession::send_command(const std::string &cmd)
    {
        if (!context_.conn_ || !context_.conn_->is_connected()) {
            return false;
        }
        std::cout << "ftp send mode=" << static_cast<int>(work_mode_) << " cmd=" << cmd;
        context_.conn_->get_output_linked_buffer()->get_current_buffer()->write_string(cmd);
        context_.conn_->flush();
        return true;
    }

    void FtpSession::change_cwd(const std::string &filepath) { context_.cwd_ = filepath.empty() ? "/" : filepath; }
    void FtpSession::on_error(int errcode) { (void)errcode; }

    bool FtpSession::set_work_file(FtpFileInfo *info)
    {
        return info && context_.conn_ && context_.file_stream_ && context_.file_stream_->set_work_file(info, context_.conn_->get_remote_address().get_ip());
    }

    void FtpSession::on_file_stream_close(FtpFileStream *ffs)
    {
        if (context_.file_stream_ == ffs) {
            context_.file_stream_ = nullptr;
        }
    }
}
