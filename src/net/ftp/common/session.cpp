#include "net/ftp/common/session.h"
#include "net/ftp/client/client_file_stream.h"
#include "net/ftp/common/def.h"
#include "net/ftp/handler/ftp_app.h"
#include "net/ftp/common/file_stream.h"
#include "net/ftp/server/server_file_stream.h"
#include "net/ftp/common/file_stream_session.h"

#include <cassert>
#include <iostream>
#include <string>

namespace net::ftp 
{
    FtpSessionContext::FtpSessionContext() : file_stream_(nullptr)
    {

    }

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
            file_stream_->quit(conn_->get_remote_address());
            file_stream_ = nullptr;
        }

        if (app_) {
            app_->on_session_closed(instance_);
            app_ = nullptr;
            conn_ = nullptr;
            return;
        }

        if (conn_) {
            conn_->close();
            conn_ = nullptr;
        }
    }

    FtpSession::FtpSession(Connection *conn, FtpApp *app, WorkMode mode, bool keepUtilSent) : work_mode_(mode), keep_util_sent_(keepUtilSent), close_(false)
    {
        context_.conn_ = conn;
        context_.app_ = app;
        context_.conn_->set_connection_handler(this);
        context_.instance_ = this;
    }

    FtpSession::~FtpSession()
    {
        close_ = true;
        context_.close();
        std::cout << "ftp session closed!\n";
    }

    void FtpSession::on_connected(Connection *conn)
    {
        conn->get_input_buff()->resize(1024 * 1024 * 2);
    }

    void FtpSession::on_error(Connection *conn)
    {

    }

    void FtpSession::on_write(Connection *conn)
    {
        
    }

    void FtpSession::on_close(Connection *conn)
    {
        if (close_ || keep_util_sent_) {
            return;
        }
        delete this;
    }

    void FtpSession::on_timer(timer::Timer *timer)
    {
        close_ = true;
        if (!keep_util_sent_) {
            delete this;
        }
    }

    void FtpSession::on_opened(FtpFileStreamSession *fs)
    {
        std::cout << "file stream opened!\n";
        /*if (work_mode_ == WorkMode::server) {
            context_.conn_->get_output_buff()->write_string("OK");
        }*/
    }

    void FtpSession::on_connect_timeout(FtpFileStreamSession *fs)
    {
        check_file_stream(fs);
    }

    void FtpSession::on_start(FtpFileStreamSession *fs)
    {
        // TODO log
    }

    void FtpSession::on_error(FtpFileStreamSession *fs)
    {
        
    }

    void FtpSession::on_completed(FtpFileStreamSession *fs)
    {
        if (context_.file_manager_.is_completed()) {
            context_.file_manager_.reset();
        }
    }

    void FtpSession::on_closed(FtpFileStreamSession *fs)
    {
        check_file_stream(fs);
    }

    void FtpSession::on_idle_timeout(FtpFileStreamSession *fs)
    {
        check_file_stream(fs);
    }

    void FtpSession::quit()
    {
        delete this;
    }

    bool FtpSession::login()
    {
        if (context_.login_success()) {
            return false;
        }

        if (!context_.user_.username_.empty() && !context_.user_.password_.empty()) {
            // TODO check passwd
        }

        return true;
    }

    void FtpSession::set_username(const std::string &username)
    {
        context_.user_.username_ = username;
    }

    void FtpSession::set_password(const std::string &passwd)
    {
        context_.user_.password_ = passwd;
    }

    bool FtpSession::start_file_stream(const InetAddress &addr, StreamMode mode)
    {
        assert(context_.app_);
        if (mode == StreamMode::Receiver) {
            context_.file_stream_ = new ServerFtpFileStream(this);
        } else {
            context_.file_stream_ = new ClientFtpFileStream(this);
        }
        return context_.file_stream_->start(addr);
    }

    void FtpSession::check_file_stream(FtpFileStreamSession *fs)
    {
        assert(context_.file_stream_);
        if (fs->get_connection()) {
            context_.file_stream_->quit(fs->get_connection()->get_remote_address());
        }
    }

    bool FtpSession::send_command(const std::string &cmd)
    {
        if (!context_.conn_ && !context_.conn_->is_connected()) {
            return false;
        }

        context_.conn_->get_output_buff()->write_string(cmd);
        context_.conn_->send();
        
        return true;
    }

    void FtpSession::change_cwd(const std::string &filepath)
    {
        context_.cwd_ = filepath;
    }

    void FtpSession::on_error(int errcode)
    {

    }

    bool FtpSession::set_work_file(FtpFileInfo *info)
    {
        if (!info || !context_.conn_) {
            return false;
        }
        return context_.file_stream_->set_work_file(info, context_.conn_->get_remote_address().get_ip());
    }

    void FtpSession::on_file_stream_close(FtpFileStream *ffs)
    {
        assert(context_.file_stream_ == ffs);
        context_.file_stream_ = nullptr;
    }
}