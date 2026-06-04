#include "common/session.h"
#include "common/def.h"
#include "common/file_stream.h"
#include "common/file_stream_session.h"
#include "handler/ftp_app.h"
#include "net/async/async_listener_host.h"
#include "server/context.h"
#include "server/server_file_stream.h"
#include "base/owner_ptr.h"

#include <cassert>
#include <filesystem>
#include "logger.h"

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

    FtpSessionContext::FtpSessionContext()
        : instance_(nullptr), file_stream_(nullptr), app_(nullptr), conn_(nullptr)
    {
    }

    FtpSessionContext::~FtpSessionContext()
    {
    }

    void FtpSessionContext::close()
    {
        if (file_stream_) {
            file_stream_->quit(conn_ ? conn_->get_remote_address() : InetAddress{});
            file_stream_.reset();
        }
        Connection *conn = conn_;
        conn_ = nullptr;
        passive_addr_.reset();
        active_addr_.reset();
        if (app_) {
            auto *app = app_;
            app_ = nullptr;
            app->on_session_closed(instance_);
        }
        if (conn) {
            conn->set_connection_handler(std::shared_ptr<ConnectionHandler>{});
            conn->close();
        }
    }

    FtpSession::FtpSession(Connection * conn, FtpApp * app, WorkMode mode, bool keepUtilSent, bool async_mode)
        : work_mode_(mode), keep_util_sent_(keepUtilSent), close_(false), async_mode_(async_mode)
    {
        conn_owner_.reset(conn, [](Connection *) {});
        context_.conn_ = conn;
        context_.app_ = app;
        if (!async_mode_) {
            context_.conn_->set_connection_handler(make_non_owning_handler(this));
        }
        context_.instance_ = this;
        if (work_mode_ == WorkMode::server) {
            context_.root_dir_ = normalize_dir(ServerContext::get_instance()->get_server_work_dir());
        } else {
            context_.root_dir_ = normalize_dir("");
        }
        context_.cwd_ = "/";
    }

    FtpSession::FtpSession(const std::shared_ptr<Connection> &conn, FtpApp *app, WorkMode mode, bool keepUtilSent, bool async_mode)
        : work_mode_(mode), keep_util_sent_(keepUtilSent), close_(false), async_mode_(async_mode), conn_owner_(conn)
    {
        context_.conn_ = yuan::base::owner_ptr(conn_owner_);
        context_.app_ = app;
        if (!async_mode_) {
            context_.conn_->set_connection_handler(make_non_owning_handler(this));
        }
        context_.instance_ = this;
        if (work_mode_ == WorkMode::server) {
            context_.root_dir_ = normalize_dir(ServerContext::get_instance()->get_server_work_dir());
        } else {
            context_.root_dir_ = normalize_dir("");
        }
        context_.cwd_ = "/";
    }

    FtpSession::~FtpSession()
    {
        if (async_mode_) {
            context_.conn_ = nullptr;
            context_.app_ = nullptr;
            context_.file_stream_.reset();
            context_.passive_addr_.reset();
            context_.active_addr_.reset();
        } else {
            context_.close();
        }
        LOG_DEBUG("ftp session closed");
    }

    void FtpSession::on_connected(const std::shared_ptr<Connection> &conn)
    {
        if (conn) {
            on_connected(*conn);
        }
    }

    void FtpSession::on_connected(Connection & conn)
    {
        (void)conn;
    }

    void FtpSession::on_error(const std::shared_ptr<Connection> &conn)
    {
        if (conn) {
            on_error(*conn);
        }
    }

    void FtpSession::on_error(Connection & conn)
    {
        (void)conn;
    }

    void FtpSession::on_read(const std::shared_ptr<Connection> &conn)
    {
        if (conn) {
            on_read(*conn);
        }
    }

    void FtpSession::on_write(const std::shared_ptr<Connection> &conn)
    {
        if (conn) {
            on_write(*conn);
        }
    }

    void FtpSession::on_write(Connection & conn)
    {
        (void)conn;
    }

    void FtpSession::on_close(const std::shared_ptr<Connection> &conn)
    {
        if (conn) {
            on_close(*conn);
        }
    }

    void FtpSession::on_close(Connection & conn)
    {
        (void)conn;
        if (async_mode_) {
            return;
        }
        if (close_) {
            return;
        }
        close_ = true;
        auto *app = context_.app_;
        auto *instance = this;
        auto file_stream = context_.file_stream_;
        auto pending_ffs = pending_file_stream_cleanup_;
        InetAddress remote_addr = context_.conn_ ? context_.conn_->get_remote_address() : InetAddress{};
        context_.app_ = nullptr;
        context_.conn_ = nullptr;
        context_.passive_addr_.reset();
        context_.active_addr_.reset();
        context_.file_stream_.reset();
        pending_file_stream_cleanup_.reset();
        if (app) {
            app->get_runtime()->dispatch([app, instance, file_stream, pending_ffs, remote_addr]() {
                if (file_stream) {
                    file_stream->quit(remote_addr);
                }
                if (pending_ffs && pending_ffs != file_stream) {
                    pending_ffs->quit(remote_addr);
                }
                app->on_session_closed(instance);
            });
        }
    }

    void FtpSession::on_input_shutdown(Connection & conn)
    {
        (void)conn;
    }

    void FtpSession::on_timer(timer::Timer * timer)
    {
        (void)timer;
        if (async_mode_ || close_) {
            return;
        }
        quit();
    }

    void FtpSession::on_opened(FtpFileStreamSession * fs)
    {
        (void)fs;
        if (async_mode_)
            return;
    }

    void FtpSession::on_connect_timeout(FtpFileStreamSession * fs)
    {
        if (async_mode_)
            return;
        if (work_mode_ == WorkMode::server) {
            send_command("425 Can't open data connection.\r\n");
        }
        check_file_stream(fs);
        clear_passive_addr();
    }

    void FtpSession::on_start(FtpFileStreamSession * fs)
    {
        (void)fs;
    }

    void FtpSession::on_error(FtpFileStreamSession * fs)
    {
        if (async_mode_)
            return;
        (void)fs;
        if (work_mode_ == WorkMode::server) {
            send_command("426 Connection closed; transfer aborted.\r\n");
        }
        clear_passive_addr();
    }

    void FtpSession::on_completed(FtpFileStreamSession * fs)
    {
        if (async_mode_)
            return;
        (void)fs;
        if (work_mode_ == WorkMode::server) {
            send_command("226 Transfer complete.\r\n");
        }
        if (context_.file_manager_.is_completed()) {
            context_.file_manager_.reset();
        }
        pending_file_stream_cleanup_ = context_.file_stream_;
        context_.file_stream_.reset();
        clear_passive_addr();
    }

    void FtpSession::on_closed(FtpFileStreamSession * fs)
    {
        if (async_mode_)
            return;
        if (pending_file_stream_cleanup_ && fs) {
            auto ffs = pending_file_stream_cleanup_;
            pending_file_stream_cleanup_.reset();
            auto *app = context_.app_;
            if (app) {
                app->get_runtime()->dispatch([ffs, fs]() {
                    ffs->quit(fs->get_remote_address());
                });
            }
        } else {
            check_file_stream(fs);
        }
    }
    void FtpSession::on_idle_timeout(FtpFileStreamSession * fs)
    {
        if (async_mode_)
            return;
        check_file_stream(fs);
        clear_passive_addr();
    }
    void FtpSession::quit()
    {
        if (close_) {
            return;
        }
        close_ = true;

        if (async_mode_) {
            data_listener_.reset();
            pending_file_info_ = nullptr;
            context_.conn_ = nullptr;
            context_.app_ = nullptr;
            context_.file_stream_.reset();
            context_.passive_addr_.reset();
            context_.active_addr_.reset();
            return;
        }

        Connection *conn = context_.conn_;
        FtpApp *app = context_.app_;
        auto ffs = context_.file_stream_;
        auto pending_ffs = pending_file_stream_cleanup_;
        InetAddress remote_addr = conn ? conn->get_remote_address() : InetAddress{};

        context_.conn_ = nullptr;
        context_.app_ = nullptr;
        context_.file_stream_.reset();
        context_.passive_addr_.reset();
        context_.active_addr_.reset();
        pending_file_stream_cleanup_.reset();

        if (ffs) {
            ffs->quit(remote_addr);
        }
        if (pending_ffs && pending_ffs != ffs) {
            pending_ffs->quit(remote_addr);
        }

        if (conn && app) {
            app->get_runtime()->dispatch([conn, app, this]() {
                app->on_session_closed(this);
                conn->set_connection_handler(std::shared_ptr<ConnectionHandler>{});
                conn->close();
            });
        } else if (conn) {
            conn->set_connection_handler(std::shared_ptr<ConnectionHandler>{});
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

    void FtpSession::set_username(const std::string & username)
    {
        context_.user_.username_ = username;
        context_.user_.logined_ = false;
    }
    void FtpSession::set_password(const std::string & passwd)
    {
        context_.user_.password_ = passwd;
        context_.user_.logined_ = false;
    }

    const std::string &FtpSession::get_username() const
    {
        return context_.user_.username_;
    }

    const std::string &FtpSession::get_password() const
    {
        return context_.user_.password_;
    }

    bool FtpSession::start_file_stream(const InetAddress & addr, StreamMode mode)
    {
        assert(context_.app_);
        if (async_mode_) {
            if (data_listener_) {
                return true;
            }
data_listener_ = std::make_unique<net::AsyncListenerHost>();
        auto *runtime = context_.app_->get_runtime();
        const auto &host = addr.get_ip();
        bool bound = host.empty() ? data_listener_->bind(addr.get_port(), *runtime)
                                  : data_listener_->bind(host, addr.get_port(), *runtime);
        if (!bound) {
                data_listener_.reset();
                return false;
            }
            return true;
        }
        if (context_.file_stream_) {
            return true;
        }
        context_.file_stream_ = std::make_shared<ServerFtpFileStream>(this);
        if (!context_.file_stream_->start(addr)) {
            context_.file_stream_.reset();
            return false;
        }
        return true;
    }

    void FtpSession::set_passive_addr(const InetAddress & addr)
    {
        context_.passive_addr_ = addr;
    }
    void FtpSession::clear_passive_addr()
    {
        context_.passive_addr_.reset();
    }

    void FtpSession::set_active_addr(const InetAddress & addr)
    {
        context_.active_addr_ = addr;
    }

    void FtpSession::clear_active_addr()
    {
        context_.active_addr_.reset();
    }

    void FtpSession::check_file_stream(FtpFileStreamSession * fs)
    {
        if (context_.file_stream_ && fs) {
            context_.file_stream_->quit(fs->get_remote_address());
        }
    }

    bool FtpSession::send_command(const std::string & cmd)
    {
        if (async_mode_) {
            return false;
        }
        if (!context_.conn_ || !context_.conn_->is_connected()) {
            return false;
        }
        LOG_DEBUG("ftp send mode={} cmd={}", static_cast<int>(work_mode_), cmd);
        context_.conn_->append_output(cmd);
        context_.conn_->flush();
        return true;
    }

    void FtpSession::change_cwd(const std::string & filepath)
    {
        context_.cwd_ = filepath.empty() ? "/" : filepath;
    }
    void FtpSession::on_error(int errcode)
    {
        (void)errcode;
    }

    bool FtpSession::set_work_file(FtpFileInfo * info)
    {
        if (async_mode_) {
            pending_file_info_ = info;
            return info != nullptr;
        }
        return info && context_.conn_ && context_.file_stream_ && context_.file_stream_->set_work_file(info, context_.conn_->get_remote_address().get_ip());
    }

    void FtpSession::on_file_stream_close(FtpFileStream * ffs)
    {
        if (context_.file_stream_ && &*context_.file_stream_ == ffs) {
            context_.file_stream_.reset();
        }
    }

    void FtpSession::detach_async()
    {
        context_.conn_ = nullptr;
        context_.app_ = nullptr;
        context_.file_stream_.reset();
        context_.passive_addr_.reset();
        context_.active_addr_.reset();
        data_listener_.reset();
        pending_file_info_ = nullptr;
    }
}
