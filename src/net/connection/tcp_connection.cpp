#include "net/connection/tcp_connection.h"
#include "net/connection/connection.h"
#include "net/handler/accept_handler.h"
#include "net/handler/tcp_socket_handler.h"
#include "net/http/header_key.h"

#include <iostream>
#include <sys/socket.h>
#include <unistd.h>

namespace net
{
    TcpConnection::TcpConnection(std::shared_ptr<net::InetAddress> remoteAddr, std::shared_ptr<net::InetAddress> localAddr, std::shared_ptr<net::Channel> channel)
    {
        this->remote_addr_ = remoteAddr;
        this->local_addr_ = localAddr;
        this->channel_ = channel;

        this->channel_->set_handler(this);

        input_buffer_ = std::make_shared<Buffer>();
        output_buffer_ = std::make_shared<Buffer>();

        closed = false;
    }

    TcpConnection::~TcpConnection()
    {
        std::cout << "free =====> !!!\n";
    }

    bool TcpConnection::is_connected()
    {
        return true;
    }

    const InetAddress & TcpConnection::get_remote_address() const
    {
        return *remote_addr_.get();
    }

    const InetAddress & TcpConnection::get_local_address() const
    {
        return *local_addr_.get();
    }

    void TcpConnection::send(Buffer *buff)
    {
        int fd = this->channel_->get_fd();
        ::send(fd, buff->begin(), buff->remain_bytes(), 0);
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
        input_buffer_->rewind();

        // TODO read data
        int fd = channel_->get_fd();
        bool read = false;
        while (true) {
            int bytes = recv(fd, input_buffer_->begin(), input_buffer_->writable_size(), 0);
            if (bytes <= 0) {
                if (bytes == 0) {
                    closed = true;
                    break;
                } else if (bytes < 0) {
                    if (errno != ENOTCONN && errno == ECONNREFUSED) {
                        std::cout << "on error!!\n";
                        tcpSocketHandler_->on_close(this);
                        closed = true;
                    }
                    break;
                }
            } else {
                read = true;
                input_buffer_->fill(bytes);
                input_buffer_->resize();
            }
        }

        tcpSocketHandler_->on_read(this);

        if (closed) {
            close();
        }
    }

    void TcpConnection::on_write_event()
    {
        // TODO write data

    }

    int TcpConnection::get_fd()
    {
        return 0;
    }
}