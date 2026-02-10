#include "net/connection/tcp_connection.h"
#include "buffer/buffer.h"
#include "buffer/pool.h"
#include "net/channel/channel.h"
#include "net/connection/connection.h"
#include "net/handler/connection_handler.h"
#include "net/socket/socket_ops.h"
#include "net/handler/event_handler.h"
#include "net/socket/socket.h"

#include <cassert>
#include <iostream>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace yuan::net
{
    TcpConnection::TcpConnection(const std::string ip, int port, int fd) : Connection()
    {
        socket_ = new Socket(ip.c_str(), port, false, fd);
        init();
    }

    TcpConnection::TcpConnection(Socket *sock) : socket_(sock), Connection()
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

        state_ = ConnectionState::connecting;
        ssl_handler_ = nullptr;
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

    void TcpConnection::write(buffer::Buffer * buff)
    {
        if (!buff || state_ != ConnectionState::connected) {
            return;
        }

        output_buffer_.append_buffer(buff);
        if (output_buffer_.get_current_buffer()->empty()) {
            output_buffer_.free_current_buffer();
        }

        channel_->enable_write();
        eventHandler_->update_channel(channel_);
    }

    void TcpConnection::write_and_flush(buffer::Buffer *buff)
    {
        if (!buff || state_ != ConnectionState::connected) {
            return;
        }

        write(buff);
        flush();
    }

    void TcpConnection::flush()
    {
        assert(state_ == ConnectionState::connected || state_ == ConnectionState::closing);

        if (output_buffer_.get_current_buffer()->empty()) {
            return;
        }

        std::size_t sz = output_buffer_.get_size();
        for (int i = 0; i < sz;) {
            int ret;
            if (ssl_handler_) {
                ret = ssl_handler_->ssl_write(output_buffer_.get_current_buffer());
            } else {
            #ifdef _WIN32
                ret = ::send(channel_->get_fd(), output_buffer_.get_current_buffer()->peek(), output_buffer_.get_current_buffer()->readable_bytes(), 0);
            #else
                ret = ::send(channel_->get_fd(), output_buffer_.get_current_buffer()->peek(), output_buffer_.get_current_buffer()->readable_bytes(), MSG_NOSIGNAL);
            #endif
            }
        
            if (ret > 0) {
                if (ret >= output_buffer_.get_current_buffer()->readable_bytes()) {
                    output_buffer_.free_current_buffer();
                    ++i;
                } else {
                    output_buffer_.get_current_buffer()->add_read_index(ret);
                    //std::cout << "still remains data: " << output_buffer_.get_current_buffer()->readable_bytes() << " bytes.\n";
                }
            } else if (ret < 0) {
                if (EAGAIN != errno && EWOULDBLOCK != errno) {
                    connectionHandler_->on_error(this);
                    close();
                    return;
                }

                channel_->enable_write();
                eventHandler_->update_channel(channel_);
                break;
            } else {
                break;
            }
        }

        if (output_buffer_.get_size() == 0) {
            if (state_ == ConnectionState::closing) {
                do_close();
            } else {
                channel_->disable_write();
                eventHandler_->update_channel(channel_);
            }
        }
    }

    // 丢弃所有未发送的数据
    void TcpConnection::abort()
    {
        do_close();
    }

    // 发送完数据后返回
    void TcpConnection::close()
    {
        ConnectionState lastState = state_;
        state_ = ConnectionState::closing;
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
        // 第一次可读可写表示连接已经建立
        if (state_ == ConnectionState::connecting && connectionHandler_) {
            state_ = ConnectionState::connected;
            connectionHandler_->on_connected(this);
        }
        
        bool read = false, close = false;
        int bytes = 0;

        input_buffer_->reset();

        do {
            if (read && input_buffer_->full()) {
                connectionHandler_->on_error(this);
                close = true;
                break;
            }

            if (!ssl_handler_) {
                #ifndef _WIN32
                    bytes = ::read(channel_->get_fd(), input_buffer_->buffer_begin(), input_buffer_->writable_size());
                #else
                    bytes = ::recv(channel_->get_fd(), input_buffer_->buffer_begin(), input_buffer_->writable_size(), 0);
                #endif
            } else {
                bytes = ssl_handler_->ssl_read(input_buffer_);
            }
        
            if (bytes <= 0) {
                if (bytes == 0) {
                    close = true;
                } else if (bytes == -1) {
                    if (errno != EINTR && errno != EWOULDBLOCK && errno != EAGAIN) {
                    #ifdef _WIN32
                        if (errno == ENOENT && WSAGetLastError() == WSAEWOULDBLOCK) {
                            goto again;
                        }
                    #endif

                        std::cerr << "read error: " << errno << std::endl;
                        connectionHandler_->on_error(this);
                        close = true;
                        break; 
                    }
                again:
                    channel_->enable_read();
                    eventHandler_->update_channel(channel_);
                    break;
                }
            } else {
                read = true;
                input_buffer_->fill(bytes);
            }
        } while (bytes > 0 && input_buffer_->full());

        if (close) {
            abort();
        } else if (read) {
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

        if ((state_ == ConnectionState::connected || state_ == ConnectionState::closing) && connectionHandler_) {
            connectionHandler_->on_write(this);
            if (state_ == ConnectionState::closing) {
                do_close();
                return;
            }
            flush();
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

    void TcpConnection::forward(Connection *conn)
    {
        conn->write(get_input_buff(true));
    }

    void TcpConnection::set_ssl_handler(std::shared_ptr<SSLHandler> sslHandler)
    {
        ssl_handler_ = sslHandler;
    }
}