#include "net/base/connection/tcp_connection.h"
#include "buffer/buffer.h"
#include "net/base/channel/channel.h"
#include "net/base/connection/connection.h"
#include "net/base/handler/connection_handler.h"
#include "net/base/socket/socket_ops.h"
#include "net/base/handler/event_handler.h"
#include "net/base/socket/socket.h"

#include <cassert>
#include <iostream>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace net
{
    TcpConnection::TcpConnection(const std::string ip, int port, int fd)
    {
        socket_ = new Socket(ip.c_str(), port, false, fd);
        init();
    }

    TcpConnection::TcpConnection(Socket *sock) : socket_(sock)
    {
        init();
    }

    void TcpConnection::init()
    {
        socket::set_none_block(socket_->get_fd(), true);
        socket::set_keep_alive(socket_->get_fd(), true);
        socket::set_no_delay(socket_->get_fd(), true);

        channel_ = new Channel(socket_->get_fd());
        channel_->set_handler(this);
        channel_->enable_read();
        channel_->enable_write();

        closed_ = false;
        state_ = ConnectionState::connecting;
    }

    TcpConnection::~TcpConnection()
    {
        state_ = ConnectionState::closed;
        assert(channel_);
        channel_->disable_all();
        channel_->set_handler(nullptr);

        if (eventHandler_) {
            eventHandler_->close_channel(channel_);
            eventHandler_ = nullptr;
        }

        if (connectionHandler_) {
            connectionHandler_->on_close(this);
            connectionHandler_ = nullptr;
        }

        if (socket_) {
            std::cout << "connection closed, ip: " << socket_->get_address()->get_ip() << ", port: " << socket_->get_address()->get_port() 
                    << ", fd: " << channel_->get_fd() << "\n";
            
            delete socket_;
            socket_ = nullptr;
        }

        channel_ = nullptr;
    }

    ConnectionState TcpConnection::get_connection_state()
    {
        return state_;
    }

    bool TcpConnection::is_connected()
    {
        return state_ == ConnectionState::connected;
    }

    const InetAddress & TcpConnection::get_remote_address()
    {
        return *socket_->get_address();
    }

    Buffer * TcpConnection::get_input_buff(bool take)
    {
        return take ? input_buffer_.take_current_buffer() : input_buffer_.get_current_buffer();
    }

    Buffer * TcpConnection::get_output_buff(bool take)
    {
        return take ? output_buffer_.take_current_buffer() : output_buffer_.get_current_buffer();
    }

    void TcpConnection::write(Buffer * buff)
    {
        if (!buff || closed_) {
            return;
        }

        output_buffer_.append_buffer(buff);
        if (output_buffer_.get_current_buffer()->empty()) {
            output_buffer_.get_current_buffer()->reset();
            output_buffer_.free_current_buffer();
        }
    }

    void TcpConnection::write_and_flush(Buffer *buff)
    {
        if (!buff || closed_) {
            return;
        }

        write(buff);
        send();
    }

    void TcpConnection::send()
    {
        if (output_buffer_.get_current_buffer()->empty()) {
            return;
        }

        std::size_t sz = output_buffer_.get_size();
        for (int i = 0; i < sz;) {
        #ifdef _WIN32
            int ret = ::send(channel_->get_fd(), output_buffer_.get_current_buffer()->peek(), output_buffer_.get_current_buffer()->readable_bytes(), 0);
        #else
            int ret = ::send(channel_->get_fd(), output_buffer_.get_current_buffer()->peek(), output_buffer_.get_current_buffer()->readable_bytes(), MSG_NOSIGNAL);
        #endif
            if (ret > 0) {
                if (ret >= output_buffer_.get_current_buffer()->readable_bytes()) {
                    output_buffer_.get_current_buffer()->reset();
                    output_buffer_.free_current_buffer();
                } else {
                    output_buffer_.get_current_buffer()->add_read_index(ret);
                    std::cout << "still remains data: " << output_buffer_.get_current_buffer()->readable_bytes() << " bytes.\n";
                    return;
                }
                ++i;
            } else if (ret < 0 && EAGAIN != errno) {
                connectionHandler_->on_error(this);
                abort();
                break;
            } else {
                break;
            }
        }
    }

    // 丢弃所有未发送的数据
    void TcpConnection::abort()
    {
        closed_ = true;
        do_close();
    }

    // 发送完数据后返回
    void TcpConnection::close()
    {
        ConnectionState lastState = state_;
        state_ = ConnectionState::closing;
        closed_ = true;
        if (lastState == ConnectionState::connecting || output_buffer_.get_current_buffer()->readable_bytes() > 0) {
            channel_->disable_read();
            eventHandler_->update_channel(channel_);
            return;
        }
        do_close();
    }

    ConnectionType TcpConnection::get_conn_type()
    {
        return ConnectionType::TCP;
    }

    Channel * TcpConnection::get_channel()
    {
        return channel_;
    }

    void TcpConnection::set_connection_handler(ConnectionHandler *handler)
    {
        this->connectionHandler_ = handler;
    }

    void TcpConnection::on_read_event()
    {
        input_buffer_.get_current_buffer()->reset();

        bool read = false;
    #ifndef _WIN32
        int bytes = ::read(channel_->get_fd(), input_buffer_.get_current_buffer()->buffer_begin(), input_buffer_.get_current_buffer()->writable_size());
    #else
        int bytes = ::recv(channel_->get_fd(), input_buffer_.get_current_buffer()->buffer_begin(), input_buffer_.get_current_buffer()->writable_size(), 0);
    #endif
        if (bytes <= 0) {
            if (bytes == 0) {
                closed_ = true;
            } else if (bytes == -1) {
                if (errno != EINTR && errno != EWOULDBLOCK && errno != EAGAIN) {
                    connectionHandler_->on_error(this);
                    closed_ = true;
                }
            }
        } else {
            read = true;
            input_buffer_.get_current_buffer()->fill(bytes);
        }

        if (closed_) {
            abort();
        } else if (read) {
            // 第一次可读可写表示连接已经建立
            if (state_ == ConnectionState::connecting && connectionHandler_) {
                state_ = ConnectionState::connected;
                connectionHandler_->on_connected(this);
            }

            if (state_ == ConnectionState::connected && connectionHandler_) {
                connectionHandler_->on_read(this);
            }
        }
    }

    void TcpConnection::on_write_event()
    {
        // 第一次可读可写表示连接已经建立
        if (state_ == ConnectionState::connecting && connectionHandler_) {
            state_ = ConnectionState::connected;
            connectionHandler_->on_connected(this);
        }

        if (state_ == ConnectionState::connected && connectionHandler_ && !closed_) {
            connectionHandler_->on_write(this);
            if (output_buffer_.get_current_buffer()->readable_bytes() > 0) {
                send();
            }
        }
    }

    void TcpConnection::set_event_handler(EventHandler *eventHandler)
    {
        assert(channel_);
        eventHandler_ = eventHandler;
        eventHandler_->update_channel(channel_);
    }

    void TcpConnection::do_close()
    {
        delete this;
    }

    ConnectionHandler * TcpConnection::get_connection_handler()
    {
        return connectionHandler_;
    }
}