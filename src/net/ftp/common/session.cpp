#include "net/ftp/common/session.h"
#include "net/ftp/common/def.h"
#include "net/ftp/handler/ftp_app.h"
#include "net/ftp/common/file_stream.h"
#include <cassert>
#include <iostream>

namespace net::ftp 
{
    FtpSessionContext::FtpSessionContext() : file_stream_(nullptr)
    {

    }

    void FtpSessionContext::close()
    {

    }

    FtpSession::FtpSession(Connection *conn, FtpApp *entry, FtpSessionWorkMode workMode, bool keepUtilSent) : work_mode_(workMode), keep_util_sent_(keepUtilSent), close_(false)
    {
        context_.conn_ = conn;
        context_.app_ = entry;
        context_.conn_->set_connection_handler(this);
    }

    FtpSession::~FtpSession()
    {
        context_.close();
    }

    void FtpSession::on_connected(Connection *conn)
    {
        
    }

    void FtpSession::on_error(Connection *conn)
    {

    }

    void FtpSession::on_read(Connection *conn)
    {
        auto buff = conn->get_input_buff();
        std::string cmd(buff->peek(), buff->peek_end());
        std::cout << "cmd >>> " << cmd << '\n';

        if (work_mode_ == FtpSessionWorkMode::server) {
            conn->write_and_flush(conn->get_input_buff(true));
        }
    }

    void FtpSession::on_write(Connection *conn)
    {
            
    }

    void FtpSession::on_close(Connection *conn)
    {
        close_ = true;
        if (!keep_util_sent_) {
            delete this;
        }
    }

    void FtpSession::on_timer(timer::Timer *timer)
    {
        close_ = true;
        if (!keep_util_sent_) {
            delete this;
        }
    }

    void FtpSession::on_opened(FtpFileStream *fs)
    {
        if (auto file = context_.file_manager_.get_next_file()) {
            stream_->set_work_file(file); 
        } else {
            stream_->quit();
            std::cerr << "-------------> error occured!!!!\n";
        }
    }

    void FtpSession::on_connect_timeout(FtpFileStream *fs)
    {
        // TODO log
        assert(stream_);
    }

    void FtpSession::on_start(FtpFileStream *fs)
    {
        // TODO log
    }

    void FtpSession::on_error(FtpFileStream *fs)
    {
        // TODO log
    }

    void FtpSession::on_completed(FtpFileStream *fs)
    {
        if (context_.file_manager_.is_completed()) {
            // do someting, tells peer is being close data connection
            stream_->quit();
            return;
        }

        if (auto file = context_.file_manager_.get_next_file()) {
            stream_->set_work_file(file);
        } else {
            stream_->quit();
            std::cerr << "-------------> error occured!!!!\n";
        }
    }

    void FtpSession::on_closed(FtpFileStream *fs)
    {
        // TODO log
    }

    void FtpSession::on_idle_timeout(FtpFileStream *fs)
    {
        // TODO log
    }

    void FtpSession::quit()
    {
        delete this;
    }

    bool FtpSession::on_login(const std::string &username, std::string &passwd)
    {
        return false;
    }

    bool FtpSession::init_file_stream(const InetAddress &addr, StreamMode mode)
    {
        assert(context_.app_);
        stream_ = new FtpFileStream(addr, mode, context_.app_);
        return stream_->start();
    }

    void FtpSession::add_command(const std::string &cmd)
    {
        context_.cmd_queue_.push(cmd);
    }
}