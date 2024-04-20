#include <iostream>
#ifdef _WIN32
#include <winsock.h>
#else
#include <unistd.h>
#endif

#include "net/base/connection/udp_connection.h"
#include "net/base/connection/tcp_connection.h"
#include "net/base/handler/connection_handler.h"
#include "net/base/socket/socket.h"

namespace net
{
    UdpConnection::UdpConnection(const std::string ip, int port, int fd) : TcpConnection(ip, port, fd)
    {
    }

    UdpConnection::UdpConnection(Socket *scok) : TcpConnection(scok)
    {
    }

    void UdpConnection::send()
    {
        if (output_buffer_.get_current_buffer()->readable_bytes() == 0) {
            return;
        }

        std::size_t sz = output_buffer_.get_size();
        for (std::size_t i = 0; i < sz; ++i) {
            int ret = ::send(channel_.get_fd(), output_buffer_.get_current_buffer()->peek(), 
            output_buffer_.get_current_buffer()->readable_bytes() > UDP_DATA_LIMIT ? UDP_DATA_LIMIT : output_buffer_.get_current_buffer()->readable_bytes(), 0);
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

    void UdpConnection::on_read_event()
    {
        input_buffer_.get_current_buffer()->reset();

        bool read = false;
    #ifndef _WIN32
        int bytes = ::read(channel_.get_fd(), input_buffer_.get_current_buffer()->buffer_begin(), input_buffer_.get_current_buffer()->writable_size());
    #else
        int bytes = recv(channel_.get_fd(), input_buffer_.get_current_buffer()->buffer_begin(), input_buffer_.get_current_buffer()->writable_size(), 0);
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
            if (state_ == ConnectionState::connecting) {
                connectionHandler_->on_connected(this);
                state_ = ConnectionState::connected;
            }
            connectionHandler_->on_read(this);
        }
    }
}