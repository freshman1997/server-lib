#include "net/ftp/common/file_stream_session.h"
#include "base/time.h"
#include "net/ftp/common/session.h"
#include "buffer/pool.h"
#include "net/ftp/handler/ftp_app.h"
#include "timer/timer.h"
#include "timer/timer_manager.h"

#include <cassert>
#include <iostream>

namespace net::ftp 
{
    FtpFileStreamSession::FtpFileStreamSession(FtpSession *session)
    {
        state_ = FileSteamState::init;
        current_file_info_ = nullptr;
        conn_timer_ = nullptr;
        conn_ = nullptr;
        session_ = session;
        auto timerManager = session_->get_app()->get_timer_manager();
        assert(timerManager);
        conn_timer_ = timerManager->interval(2 * 1000, 10 * 1000, this, -1);
        last_active_time_ = 0;
        write_buff_size_ = default_write_buff_size;
    }

    FtpFileStreamSession::~FtpFileStreamSession()
    {
        std::cout << "~FtpFileStreamSession()\n";
        if (conn_timer_) {
            conn_timer_->cancel();
            conn_timer_ = nullptr;
        }

        if (session_) {
            session_->on_closed(this);
            session_ = nullptr;
        }

        if (conn_) {
            conn_->close();
            conn_ = nullptr;
        }
    }
    
    void FtpFileStreamSession::on_connected(Connection *conn)
    {
        assert(session_);
        if (conn_) {
            conn->close();
            return;
        }

        state_ = FileSteamState::connected;
        conn_ = conn;
        conn_->get_input_buff()->reset();
        if (conn_->get_input_buff()->writable_size() < default_write_buff_size) {
            conn_->get_input_buff()->resize(default_write_buff_size);
        }

        conn_->get_output_buff()->reset();
        if (conn_->get_output_buff()->writable_size() < default_write_buff_size) {
            conn_->get_output_buff()->resize(default_write_buff_size);
        }

        session_->on_opened(this);
        last_active_time_ = base::time::now();
    }

    void FtpFileStreamSession::on_error(Connection *conn)
    {
        state_ = FileSteamState::connection_error;
        assert(session_);
        session_->on_error(this);
    }

    void FtpFileStreamSession::on_read(Connection *conn)
    {
        assert(session_);
        if (state_ != FileSteamState::connected && state_ != FileSteamState::idle 
            && state_ != FileSteamState::processing) {
            session_->on_error(this);
        } else {
            if (!current_file_info_ || current_file_info_->mode_ != StreamMode::Receiver || !current_file_info_->ready_) {
                return;
            }

            state_ = FileSteamState::processing;
            float per = (current_file_info_->current_progress_ + conn->get_input_buff()->readable_bytes()) / (current_file_info_->file_size_ * 1.0);
            std::cout << ">>> receive: " << std::to_string(per * 100) << "%\n";
            int ret = current_file_info_->write_file(conn->get_input_buff());            
            if (ret < 0) {
                state_ = FileSteamState::file_error;
                session_->on_error(this);
            } else if (current_file_info_->is_completed()) {
                std::cout << "done >> " << current_file_info_->current_progress_  << ", "
                        << current_file_info_->file_size_ << '\n';
                state_ = FileSteamState::idle;
                session_->on_completed(this);
                current_file_info_ = nullptr;
                last_active_time_ = base::time::now();
            }
        }
    }

    void FtpFileStreamSession::on_write(Connection *conn)
    {
        assert(session_);
        if (state_ != FileSteamState::connected && state_ != FileSteamState::idle 
            && state_ != FileSteamState::processing) {
            session_->on_error(this);
        } else {
            if (!current_file_info_ || current_file_info_->mode_ != StreamMode::Sender || !current_file_info_->ready_) {
                return;
            }

            state_ = FileSteamState::processing;
            auto buff = conn->get_output_buff();
            bool newBuff = false;
            if (buff->readable_bytes() > 0) {
                newBuff = true;
                buff = BufferedPool::get_instance()->allocate(write_buff_size_);
            } else {
                buff->reset();
            }

            int ret = current_file_info_->read_file(write_buff_size_, buff);
            float per = (current_file_info_->current_progress_ + ret) / (current_file_info_->file_size_ * 1.0);
            std::cout << ">>> send: " << std::to_string(per * 100) << "%\n";
            if (ret <= 0) {
                state_ = FileSteamState::file_error;
                session_->on_error(this);
                if (newBuff) {
                    BufferedPool::get_instance()->free(buff);
                }
            } else {
                if (newBuff) {
                    conn->write(buff);
                }
                conn->send();
                if (session_ && current_file_info_->is_completed()) {
                    state_ = FileSteamState::idle;
                    session_->on_completed(this);
                    current_file_info_->ready_ = false;
                }
                last_active_time_ = base::time::now();
            }
        }
    }

    void FtpFileStreamSession::on_close(Connection *conn)
    {
        if (state_ == FileSteamState::disconnected) {
            return;
        }
        conn_ = nullptr;
        quit();
    }

    void FtpFileStreamSession::on_timer(timer::Timer *timer)
    {
        assert(session_);
        if (current_file_info_ && current_file_info_->state_ == FileState::processing) {
            last_active_time_ = base::time::now();
            return;
        }

        if (state_ == FileSteamState::idle) {
            if (base::time::now() - last_active_time_ >= default_session_idle_timeout) {
                state_ = FileSteamState::idle_timeout;
                session_->on_idle_timeout(this);
                quit();
            }
        } else if (state_ == FileSteamState::connecting) {
            state_ = FileSteamState::connect_timeout;
            session_->on_connect_timeout(this);
            quit();
        } else {
            state_ = FileSteamState::idle;
        }
    }

    Connection * FtpFileStreamSession::get_connection()
    {
        return conn_;
    }

    std::size_t FtpFileStreamSession::get_write_buff_size()
    {
        return write_buff_size_;
    }

    void FtpFileStreamSession::set_write_buff_size(std::size_t sz)
    {
        write_buff_size_ = sz;
    }

    void FtpFileStreamSession::set_work_file(FtpFileInfo *info)
    {
        current_file_info_ = info;
        std::cout << ">>> work file: " << info->origin_name_ << '\n';
    }

    FtpFileInfo * FtpFileStreamSession::get_work_file()
    {
        return current_file_info_;
    }

    void FtpFileStreamSession::quit()
    {
        state_ = FileSteamState::disconnected;
        delete this;
    }

    FileSteamState FtpFileStreamSession::get_state()
    {
        return state_;
    }
}