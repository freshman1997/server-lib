#include "net/base/connection/tcp_connection.h"
#include "buffer/buffer.h"
#include "net/base/connection/connection.h"
#include "net/base/handler/connection_handler.h"
#include "net/base/socket/socket_ops.h"
#include "net/base/handler/event_handler.h"
#include "net/base/socket/socket.h"

#include <iostream>
#include <unistd.h>

namespace net
{
    TcpConnection::TcpConnection(const std::string ip, int port, int fd)
    {
        socket_ = new Socket(ip.c_str(), port, fd);
        init();
    }

    TcpConnection::TcpConnection(Socket *scok) : socket_(scok)
    {
        init();
    }

    void TcpConnection::init()
    {
        socket::set_none_block(socket_->get_fd(), true);
        socket::set_keep_alive(socket_->get_fd(), true);
        socket::set_no_delay(socket_->get_fd(), true);

        channel_.set_fd(socket_->get_fd());
        channel_.set_handler(this);
        channel_.enable_read();
        channel_.enable_write();

        closed_ = false;
        state_ = ConnectionState::connecting;
    }

    TcpConnection::~TcpConnection()
    {
        channel_.disable_all();
        eventHandler_->update_event(&channel_);
        channel_.set_handler(nullptr);
        connectionHandler_->on_close(this);
        state_ = ConnectionState::closed;
        
        std::cout << "connection closed, ip: " << socket_->get_address()->get_ip() << ", port: " << socket_->get_address()->get_port() 
                << ", fd: " << channel_.get_fd() << "\n";
        
        delete socket_;
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
        if (output_buffer_.get_current_buffer()->readable_bytes() == 0) {
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
        if (output_buffer_.get_current_buffer()->readable_bytes() == 0) {
            return;
        }

        std::size_t sz = output_buffer_.get_size();
        for (int i = 0; i < sz; ++i) {
            int ret = ::send(channel_.get_fd(), output_buffer_.get_current_buffer()->peek(), output_buffer_.get_current_buffer()->readable_bytes(), 0);
            if (ret > 0) {
                if (ret >= output_buffer_.get_current_buffer()->readable_bytes()) {
                    output_buffer_.get_current_buffer()->reset();
                    output_buffer_.free_current_buffer();
                } else {
                    output_buffer_.get_current_buffer()->add_read_index(ret);
                    std::cout << "still remains data: " << output_buffer_.get_current_buffer()->readable_bytes() << " bytes.\n";
                    return;
                }
            } else if (ret < 0) {
                connectionHandler_->on_error(this);
                abort();
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
            channel_.disable_read();
            eventHandler_->update_event(&channel_);
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
        return &channel_;
    }

    void TcpConnection::set_connection_handler(ConnectionHandler *handler)
    {
        this->connectionHandler_ = handler;
    }

    void TcpConnection::on_read_event()
    {
        input_buffer_.get_current_buffer()->reset();

        bool read = false;
        int bytes = ::read(channel_.get_fd(), input_buffer_.get_current_buffer()->buffer_begin(), input_buffer_.get_current_buffer()->writable_size());
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
            if (state_ == ConnectionState::connecting) {
                connectionHandler_->on_connected(this);
                state_ = ConnectionState::connected;
            }
            connectionHandler_->on_read(this);
        }
    }

    void TcpConnection::on_write_event()
    {
        // 第一次可读可写表示连接已经建立
        if (state_ == ConnectionState::connecting) {
            connectionHandler_->on_connected(this);
            state_ = ConnectionState::connected;
        }

        connectionHandler_->on_write(this);
        if (output_buffer_.get_current_buffer()->readable_bytes() > 0) {
            send();
        }
    }

    void TcpConnection::set_event_handler(EventHandler *eventHandler)
    {
        eventHandler_ = eventHandler;
        eventHandler_->update_event(&channel_);
    }

    void TcpConnection::do_close()
    {
        delete this;
    }

    Socket * TcpConnection::get_scoket()
    {
        return socket_;
    }
}