#include "net/connection/tcp_connection.h"
#include <memory>
#include <sys/socket.h>

namespace net
{
    TcpConnection::TcpConnection(std::shared_ptr<net::InetAddress> remoteAddr, std::shared_ptr<net::InetAddress> localAddr, std::shared_ptr<net::Channel> channel)
    {
        this->remote_addr_ = remoteAddr;
        this->local_addr_ = localAddr;
        this->channel_ = channel;

        input_buffer_ = std::make_shared<Buffer>();
        output_buffer_ = std::make_shared<Buffer>();
    }

    bool TcpConnection::is_connected()
    {
        return false;
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
        
    }

    // 丢弃所有未发送的数据
    void TcpConnection::abort()
    {

    }

    // 发送完数据后返回
    void TcpConnection::close()
    {

    }

    void TcpConnection::on_read_event()
    {
        // TODO read data
        int fd = channel_->get_fd();
        char buff[1025] = {0};
        while (true) {
            int bytes = recv(fd, input_buffer_->begin(), input_buffer_->writable_size(), 0);
            if (bytes <= 0) {
                if (bytes == 0) {

                } else if (bytes < 0) {
                    
                }
                // close
                break;
            }
            
            input_buffer_->fill(bytes);
            input_buffer_->resize();
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