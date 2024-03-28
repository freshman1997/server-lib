#include "net/base/connection/tcp_connection.h"
#include "buffer/buffer.h"
#include "net/base/connection/connection.h"
#include "net/base/handler/connection_handler.h"
#include "net/base/socket/socket_ops.h"
#include "net/base/handler/event_handler.h"

#include <cassert>
#include <iostream>
#include <unistd.h>

namespace net
{
    TcpConnection::TcpConnection(const std::string ip, int port, int fd)
    {
        addr_.set_addr(ip, port);

        socket::set_none_block(fd, true);
        socket::set_keep_alive(fd, true);

        channel_.set_fd(fd);
        channel_.set_handler(this);
        channel_.enable_read();
        channel_.enable_write();

        closed_ = false;
    }

    TcpConnection::~TcpConnection()
    {
        std::cout << "connection closed, ip: " << addr_.get_ip() << ", port: " << addr_.get_port() 
                << ", fd: " << channel_.get_fd() << "\n";
    }

    bool TcpConnection::is_connected()
    {
        return true;
    }

    const InetAddress & TcpConnection::get_remote_address() const
    {
        return addr_;
    }

    Buffer * TcpConnection::get_input_buff()
    {
        return input_buffer_.get_current_buffer();
    }

    Buffer * TcpConnection::get_output_buff()
    {
        return output_buffer_.get_current_buffer();
    }

    void TcpConnection::send(Buffer * buff)
    {
        if (!buff) {
            return;
        }
        
        output_buffer_.append_buffer(buff);
    }

    void TcpConnection::send()
    {
        assert(output_buffer_.get_current_buffer()->readable_bytes() > 0);
        int ret = ::write(channel_.get_fd(), output_buffer_.get_current_buffer()->peek(), output_buffer_.get_current_buffer()->readable_bytes());
        if (ret > 0) {
            if (ret >= output_buffer_.get_current_buffer()->readable_bytes()) {
                output_buffer_.get_current_buffer()->reset();
                if (output_buffer_.get_size() > 1) {
                    output_buffer_.free_current_buffer();
                } 
            } else {
                output_buffer_.get_current_buffer()->add_read_index(ret);
                std::cout << "still remains data: " << output_buffer_.get_current_buffer()->readable_bytes() << " bytes.\n";
                return;
            }
        } else if (ret < 0) {
            closed_ = true;
        }

        if (closed_) {
            do_close();
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
        closed_ = true;
        if (output_buffer_.get_current_buffer()->readable_bytes() > 0) {
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
                    std::cout << "on error!!\n";
                    connectionHandler_->on_error(this);
                    closed_ = true;
                }
            }
        } else {
            read = true;
            input_buffer_.get_current_buffer()->fill(bytes);
        }

        if (closed_) {
            close();
        } else if (read) {
            connectionHandler_->on_read(this);
        }
    }

    void TcpConnection::on_write_event()
    {
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
        connectionHandler_->on_close(this);
        channel_.set_handler(nullptr);
        delete this;
    }
}