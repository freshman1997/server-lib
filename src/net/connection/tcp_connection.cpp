#include "net/connection/tcp_connection.h"
#include "net/connection/connection.h"
#include "net/handler/connection_handler.h"
#include "net/socket/socket_ops.h"

#include <iostream>
#include <sys/socket.h>
#include <unistd.h>

namespace net
{
    TcpConnection::TcpConnection(std::shared_ptr<net::InetAddress> remoteAddr, std::shared_ptr<net::Channel> channel)
    {
        this->addr_ = remoteAddr;
        this->channel_ = channel;

        this->channel_->set_handler(this);

        input_buffer_ = std::make_shared<Buffer>();
        output_buffer_ = std::make_shared<Buffer>();

        closed = false;

        //socket::set_none_block(this->channel_->get_fd(), true);
        socket::set_keep_alive(this->channel_->get_fd(), true);
    }

    TcpConnection::~TcpConnection()
    {
        std::cout << "TcpConnection::~TcpConnection()\n";
    }

    bool TcpConnection::is_connected()
    {
        return true;
    }

    const InetAddress & TcpConnection::get_remote_address() const
    {
        return *addr_.get();
    }

    const InetAddress & TcpConnection::get_local_address() const
    {
        return *addr_.get();
    }

    std::shared_ptr<Buffer> TcpConnection::get_input_buff()
    {
        return input_buffer_;
    }

    std::shared_ptr<Buffer> TcpConnection::get_output_buff()
    {
        return output_buffer_;
    }

    void TcpConnection::send(std::shared_ptr<Buffer> buff)
    {
        int fd = this->channel_->get_fd();
        ::write(fd, buff->begin(), buff->readable_bytes());
    }

    // 丢弃所有未发送的数据
    void TcpConnection::abort()
    {
        closed = true;
    }

    // 发送完数据后返回
    void TcpConnection::close()
    {
        connectionHandler_->on_close(this);
        delete this;
    }

    ConnectionType TcpConnection::get_conn_type()
    {
        return ConnectionType::TCP;
    }

    Channel * TcpConnection::get_channel()
    {
        return channel_.get();
    }

    void TcpConnection::set_connection_handler(ConnectionHandler *handler)
    {
        this->connectionHandler_ = handler;
    }

    void TcpConnection::on_read_event()
    {
        input_buffer_->reset();

        // TODO read data
        int fd = channel_->get_fd();
        bool read = false;
        int bytes = ::read(fd, input_buffer_->buffer_begin(), input_buffer_->writable_size());
        if (bytes <= 0) {
            if (bytes == 0) {
                closed = true;
            } else if (bytes == -1) {
                if (errno != EINTR && errno != EWOULDBLOCK && errno != EAGAIN) {
                    std::cout << "on error!!\n";
                    connectionHandler_->on_error(this);
                    closed = true;
                }

                return;
            }
        } else {
            read = true;
            input_buffer_->fill(bytes);
        }

        if (closed) {
            close();
        } else {
            connectionHandler_->on_read(this);
        }
    }

    void TcpConnection::on_write_event()
    {
        if (output_buffer_->readable_bytes() > 0) {
            send(output_buffer_);
        }
    }

    int TcpConnection::get_fd()
    {
        return channel_->get_fd();
    }
}