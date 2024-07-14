#include "net/ftp/common/file_stream.h"
#include "net/base/acceptor/tcp_acceptor.h"
#include "net/base/event/event_loop.h"
#include "net/base/socket/socket.h"
#include "net/ftp/client/config.h"
#include "net/ftp/common/def.h"
#include "net/ftp/handler/ftp_app.h"
#include "singleton/singleton.h"
#include "timer/timer.h"
#include "net/base/connection/tcp_connection.h"

#include <cassert>
#include <iostream>

namespace net::ftp 
{
    FtpFileStream::FtpFileStream(const InetAddress &addr, StreamMode mode, FtpApp *entry)
    {
        addr_ = addr;
        mode_ = mode;
        state_ = FileSteamState::init;
        current_file_info_ = nullptr;
        event_handler_ = nullptr;
        conn_timer_ = nullptr;
        conn_ = nullptr;
        entry_ = entry;
    }

    FtpFileStream::~FtpFileStream()
    {
        if (conn_timer_) {
            conn_timer_->cancel();
            conn_timer_ = nullptr;
        }

        if (event_handler_) {
            event_handler_->on_closed(this);
            event_handler_ = nullptr;
        }
    }

    void FtpFileStream::on_connected(Connection *conn)
    {
        state_ = FileSteamState::connected;
        assert(event_handler_);
        event_handler_->on_opened(this);
    }

    void FtpFileStream::on_error(Connection *conn)
    {
        state_ = FileSteamState::connection_error;
        assert(event_handler_);
        conn_ = conn;
        event_handler_->on_error(this);
    }

    void FtpFileStream::on_read(Connection *conn)
    {
        assert(event_handler_);
        conn_ = conn;
        if (state_ != FileSteamState::connected || state_ != FileSteamState::idle) {
            event_handler_->on_error(this);
        } else {
            if (!current_file_info_) {
                return;
            }

            state_ = FileSteamState::processing;
            assert(mode_ == StreamMode::Receiver);
            
            int ret = current_file_info_->write_file(conn->get_input_buff());
            if (ret < 0) {
                state_ = FileSteamState::file_error;
                event_handler_->on_error(this);
            } else if (current_file_info_->is_completed()) {
                state_ = FileSteamState::idle;
                event_handler_->on_completed(this);

                auto timerManager = entry_->get_timer_manager();
                assert(timerManager);
                if (conn_timer_) {
                    conn_timer_->cancel();
                }
                conn_timer_ = timerManager->timeout(singleton::Singleton<FtpClientConfig>().get_idle_timeout(), this);
            }
        }
    }

    void FtpFileStream::on_write(Connection *conn)
    {
        assert(event_handler_);
        conn_ = conn;
        if (state_ != FileSteamState::connected || state_ != FileSteamState::idle) {
            event_handler_->on_error(this);
        } else {
            if (!current_file_info_) {
                return;
            }

            state_ = FileSteamState::processing;
            assert(mode_ == StreamMode::Sender);

            int ret = current_file_info_->read_file(singleton::Singleton<FtpClientConfig>().get_read_amount(), conn->get_input_buff());
            if (ret <= 0) {
                state_ = FileSteamState::file_error;
                event_handler_->on_error(this);
            } else {
                conn->send();
                if (current_file_info_->is_completed()) {
                    event_handler_->on_completed(this);
                    state_ = FileSteamState::idle;

                    auto timerManager = entry_->get_timer_manager();
                    assert(timerManager);
                    if (conn_timer_) {
                        conn_timer_->cancel();
                    }
                    conn_timer_ = timerManager->timeout(singleton::Singleton<FtpClientConfig>().get_idle_timeout(), this);
                }
            }
        }
    }

    void FtpFileStream::on_close(Connection *conn)
    {
        conn_ = conn;
        delete this;
    }

    void FtpFileStream::on_timer(timer::Timer *timer)
    {
        assert(event_handler_);
        if (state_ == FileSteamState::idle) {
            state_ = FileSteamState::idle_timeout;
            event_handler_->on_idle_timeout(this);
        } else if (state_ == FileSteamState::connecting) {
            state_ = FileSteamState::connect_timeout;
            event_handler_->on_connect_timeout(this);
        }
        delete this;
    }

    void FtpFileStream::set_event_handler(FtpFileStreamEvent *eventHandler)
    {
        event_handler_ = eventHandler;
    }

    FileSteamState FtpFileStream::get_state()
    {
        return state_;
    }

    void FtpFileStream::set_work_file(FileInfo *info)
    {
        current_file_info_ = info;
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

        auto evLoop = entry_->get_event_loop();
        assert(evLoop);
        
        acceptor->set_event_handler(evLoop);
        acceptor->set_connection_handler(this);
        evLoop->update_event(acceptor->get_channel());

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

        auto evLoop = entry_->get_event_loop();
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
        delete this;
    }
}