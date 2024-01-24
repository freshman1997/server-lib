#include "net/connection/tcp_connection.h"
#include "net/connection/connection.h"
#include "net/handler/tcp_socket_handler.h"
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

    void TcpConnection::send(Buffer *buff)
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
        tcpSocketHandler_->on_close(this);
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

    void TcpConnection::set_tcp_handler(TcpConnectionHandler *tcpSocketHandler)
    {
        this->tcpSocketHandler_ = tcpSocketHandler;
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
                    tcpSocketHandler_->on_error(this);
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
            tcpSocketHandler_->on_read(this);
        }
    }

    void TcpConnection::on_write_event()
    {
        // TODO write data

    }

    int TcpConnection::get_fd()
    {
        return channel_->get_fd();
    }
}