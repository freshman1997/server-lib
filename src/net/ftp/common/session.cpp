#include "net/ftp/common/session.h"
#include "base/time.h"
#include "net/ftp/common/def.h"
#include "net/ftp/handler/ftp_app.h"
#include "net/ftp/common/file_stream.h"
#include "nlohmann/detail/value_t.hpp"
#include "nlohmann/json.hpp"
#include <_mingw_stat64.h>
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
            if (it.second.type == FtpSessionValueType::string_val) {
                delete it.second.item.sval;
            }
        }
    }

    void FtpSessionContext::close()
    {
        if (app_) {
            app_->on_session_closed(instance_);
            app_ = nullptr;
        }

        if (instance_ && instance_->close_) {
            conn_ = nullptr;
        } else if (conn_) {
            conn_->close();
            conn_ = nullptr;
        }

        if (file_stream_) {
            file_stream_->quit();
            file_stream_ = nullptr;
        }
    }

    FtpSession::FtpSession(Connection *conn, FtpApp *entry, FtpSessionWorkMode workMode, bool keepUtilSent) : work_mode_(workMode), keep_util_sent_(keepUtilSent), close_(false)
    {
        context_.conn_ = conn;
        context_.app_ = entry;
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
        
    }

    void FtpSession::on_error(Connection *conn)
    {

    }

    static bool str_cmp(const char *begin, const char *end, const char *str)
    {
        const char *p = begin;
        const char *p1 = str;
        for (;p != end && *p1; ++p, ++p1) {
            if (std::tolower(*p) != *p1) {
                return false;
            }
        }

        return p == end && !(*p1);
    }

    void FtpSession::on_read(Connection *conn)
    {
        auto buff = conn->get_input_buff();
        std::string cmd(buff->peek(), buff->peek_end());
        std::cout << "cmd >>> " << cmd << '\n';

        if (work_mode_ == FtpSessionWorkMode::server) {
            if (cmd == "setup" && context_.file_stream_ == nullptr) {
                if (start_file_stream({"", 12124}, StreamMode::Receiver)) {
                    std::cout << "file stream connection settled!\n";
                    conn->get_output_buff()->write_string("start connecting");
                    conn->send();
                } else {
                    std::cout << "file stream connection settle failed!!!\n";
                }
            } else {
                if (cmd == "ready") {
                    context_.file_stream_->get_work_file()->ready_ = true;
                } else if (cmd == "next") {
                    if (auto file = context_.file_manager_.get_next_file()) {
                        if (context_.file_stream_->get_stream_mode() == StreamMode::Sender) {
                            assert(context_.conn_);
                            context_.conn_->get_output_buff()->write_string("file: " + file->build_cmd_args());
                            context_.conn_->send();
                        }
                        context_.file_stream_->set_work_file(file);
                    } else {
                        context_.conn_->get_output_buff()->write_string("quit");
                        context_.conn_->send();
                    }
                } else {
                    conn->write_and_flush(conn->get_input_buff(true));
                }
            }
        } else {
            if (cmd == "start connecting" && context_.file_stream_ == nullptr) {
                if (start_file_stream({"192.168.96.1", 12124}, StreamMode::Sender)) {
                    std::cout << "file stream has connected to server!\n";
                } else {
                    std::cout << "file stream connect failed!!!\n";
                }
            } else if (cmd == "quit") {
                context_.file_stream_->quit();
            } else if (str_cmp(cmd.c_str(), cmd.c_str() + 6, "file: ")) {
                nlohmann::json jval;
                auto res = jval.parse(buff->peek(), buff->peek_end());
                if (res != nlohmann::detail::value_t::discarded) {
                    FileInfo info;
                    info.ready_ = true;
                    info.origin_name_ = jval["origin"];
                    info.file_size_ = jval["size"];
                    info.current_progress_ = jval["progress"];
                    info.dest_name_ = "D:/misc/" + std::to_string(base::time::now()) + ".txt";
                    context_.file_manager_.add_file(info);
                    context_.file_stream_->set_work_file(context_.file_manager_.get_next_file());
                    context_.conn_->get_output_buff()->write_string("ready");
                    context_.conn_->send();
                } else {
                    context_.file_stream_->quit();
                }
            }
        }
    }

    void FtpSession::on_write(Connection *conn)
    {
        if (work_mode_ == FtpSessionWorkMode::client) {
            if (!context_.cmd_queue_.empty()) {
                const std::string &cmd = context_.cmd_queue_.front();
                conn->get_output_buff()->write_string(cmd);
                conn->send();
                context_.cmd_queue_.pop();
            }
        }
    }

    void FtpSession::on_close(Connection *conn)
    {
        if (close_) {
            return;
        }

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
        std::cout << "file stream opened!\n";
        context_.file_manager_.reset();
        if (fs->get_stream_mode() == StreamMode::Sender) {
            context_.file_manager_.set_work_filepath("/home/yuan/1.txt");
        }

        if (auto file = context_.file_manager_.get_next_file()) {
            if (fs->get_stream_mode() == StreamMode::Sender) {
                assert(context_.conn_);
                context_.conn_->get_output_buff()->write_string("file: " + file->build_cmd_args());
                context_.conn_->send();
            }
            context_.file_stream_->set_work_file(file);
        }
    }

    void FtpSession::on_connect_timeout(FtpFileStream *fs)
    {
        check_file_stream(fs);
    }

    void FtpSession::on_start(FtpFileStream *fs)
    {
        // TODO log
    }

    void FtpSession::on_error(FtpFileStream *fs)
    {
        check_file_stream(fs);
    }

    void FtpSession::on_completed(FtpFileStream *fs)
    {
        // 发送完成，等待对方接收完毕
        if (context_.file_manager_.is_completed()) {
            // do someting, tells peer is being close data connection
            context_.file_stream_->quit();
            return;
        }

        if (auto file = context_.file_manager_.get_next_file()) {
            context_.file_stream_->set_work_file(file);
        } else {
            context_.file_stream_->quit();
            std::cerr << "-------------> error occured!!!!\n";
        }
    }

    void FtpSession::on_closed(FtpFileStream *fs)
    {
        check_file_stream(fs);
    }

    void FtpSession::on_idle_timeout(FtpFileStream *fs)
    {
        check_file_stream(fs);
    }

    void FtpSession::quit()
    {
        delete this;
    }

    bool FtpSession::on_login(const std::string &username, std::string &passwd)
    {
        return false;
    }

    bool FtpSession::start_file_stream(const InetAddress &addr, StreamMode mode)
    {
        assert(context_.app_);
        context_.file_stream_ = new FtpFileStream(addr, mode, this);
        return context_.file_stream_->start();
    }

    void FtpSession::add_command(const std::string &cmd)
    {
        context_.cmd_queue_.push(cmd);
    }

    void FtpSession::check_file_stream(FtpFileStream *fs)
    {
        context_.file_stream_ = nullptr;
    }
}