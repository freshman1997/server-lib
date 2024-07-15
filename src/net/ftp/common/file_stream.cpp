#include "net/ftp/common/file_stream.h"
#include "net/base/acceptor/tcp_acceptor.h"
#include "net/base/event/event_loop.h"
#include "net/base/socket/socket.h"
#include "net/ftp/client/config.h"
#include "net/ftp/common/def.h"
#include "net/ftp/common/session.h"
#include "net/ftp/handler/ftp_app.h"
#include "singleton/singleton.h"
#include "timer/timer.h"
#include "net/base/connection/tcp_connection.h"
#include "base/time.h"

#include <cassert>
#include <iostream>

namespace net::ftp 
{
    FtpFileStream::FtpFileStream(const InetAddress &addr, StreamMode mode, FtpSession *session)
    {
        addr_ = addr;
        mode_ = mode;
        state_ = FileSteamState::init;
        current_file_info_ = nullptr;
        conn_timer_ = nullptr;
        conn_ = nullptr;
        session_ = session;
        auto timerManager = session_->get_app()->get_timer_manager();
        assert(timerManager);
        conn_timer_ = timerManager->interval(2 * 1000, 10 * 1000, this, -1);
        last_active_time_ = 0;
    }

    FtpFileStream::~FtpFileStream()
    {
        std::cout << "~FtpFileStream()\n";
        if (conn_timer_) {
            conn_timer_->cancel();
            conn_timer_ = nullptr;
        }

        if (conn_) {
            conn_->close();
            conn_ = nullptr;
        }

        if (session_) {
            auto ptr  = session_->get_ptr_value("acceptor");
            if (ptr) {
                auto acceptor = static_cast<TcpAcceptor *>(ptr);
                acceptor->close();
                session_->remove_item("acceptor");
            }
            session_->on_closed(this);
            session_ = nullptr;
        }
    }

    void FtpFileStream::on_connected(Connection *conn)
    {
        if (conn_) {
            conn->close();
            return;
        }

        state_ = FileSteamState::connected;
        conn_ = conn;
        assert(session_);
        session_->on_opened(this);
        last_active_time_ = base::time::now();
    }

    void FtpFileStream::on_error(Connection *conn)
    {
        state_ = FileSteamState::connection_error;
        assert(session_);
        session_->on_error(this);
    }

    void FtpFileStream::on_read(Connection *conn)
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
            int ret = current_file_info_->write_file(conn->get_input_buff());
            if (ret < 0) {
                state_ = FileSteamState::file_error;
                session_->on_error(this);
            } else if (current_file_info_->is_completed()) {
                state_ = FileSteamState::idle;
                session_->on_completed(this);
                current_file_info_->ready_ = false;
                last_active_time_ = base::time::now();
            }
        }
    }

    void FtpFileStream::on_write(Connection *conn)
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
            conn->get_output_buff()->reset();
            int ret = current_file_info_->read_file(singleton::Singleton<FtpClientConfig>().get_read_amount(), conn->get_output_buff());
            if (ret <= 0) {
                state_ = FileSteamState::file_error;
                session_->on_error(this);
            } else {
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

    void FtpFileStream::on_close(Connection *conn)
    {
        if (state_ == FileSteamState::disconnected) {
            return;
        }
        conn_ = nullptr;
        quit();
    }

    void FtpFileStream::on_timer(timer::Timer *timer)
    {
        assert(session_);
        if (current_file_info_ && current_file_info_->state_ == FileState::processing) {
            last_active_time_ = base::time::now();
            return;
        }

        if (state_ == FileSteamState::idle) {
            if (base::time::now() - last_active_time_ >= singleton::Singleton<FtpClientConfig>().get_idle_timeout()) {
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

    FileSteamState FtpFileStream::get_state()
    {
        return state_;
    }

    StreamMode FtpFileStream::get_stream_mode()
    {
        return mode_;
    }

    void FtpFileStream::set_work_file(FileInfo *info)
    {
        current_file_info_ = info;
        std::cout << ">>> work file: " << info->origin_name_ << '\n';
    }

    FileInfo * FtpFileStream::get_work_file()
    {
        return current_file_info_;
    }

    Connection * FtpFileStream::get_cur_connection()
    {
        return conn_;
    }

    bool FtpFileStream::serve()
    {
        assert(session_);
        Socket *sock = new Socket("", addr_.get_port());
        if (!sock->valid()) {
            std::cerr << "cant create socket file descriptor!\n";
            delete sock;
            return false;
        }

        if (!sock->bind()) {
            std::cerr << "cant bind port: " << addr_.get_port() << "!\n";
            delete sock;
            return false;
        }

        TcpAcceptor *acceptor = new TcpAcceptor(sock);
        if (!acceptor->listen()) {
            std::cout << "cant listen on port: " << addr_.get_port() << "!\n";
            delete acceptor;
            return false;
        }

        auto evLoop = session_->get_app()->get_event_loop();
        assert(evLoop);
        
        acceptor->set_event_handler(evLoop);
        acceptor->set_connection_handler(this);

        session_->set_item_value<void *>("acceptor", static_cast<void *>(acceptor));

        return true;
    }

    bool FtpFileStream::connect()
    {
        net::Socket *sock = new net::Socket(addr_.get_ip().c_str(), addr_.get_port());
        if (!sock->valid()) {
            std::cerr << "create socket fail!!\n";
            return false;
        }

        sock->set_none_block(true);
        if (!sock->connect()) {
            std::cerr << " connect failed " << std::endl;
            return false;
        }

        TcpConnection *conn = new TcpConnection(sock);
        conn->set_connection_handler(this);

        auto evLoop = session_->get_app()->get_event_loop();
        assert(evLoop);

        evLoop->update_event(conn->get_channel());
        conn->set_event_handler(evLoop);

        return true;
    }

    bool FtpFileStream::start()
    {
        return mode_ == StreamMode::Receiver ? serve() : connect();
    }

    void FtpFileStream::quit()
    {
        state_ = FileSteamState::disconnected;
        delete this;
    }
}