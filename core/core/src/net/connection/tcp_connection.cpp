#include "net/connection/tcp_connection.h"
#include "net/channel/channel.h"
#include "net/connection/connection.h"
#include "net/handler/connection_handler.h"
#include "net/socket/socket_ops.h"
#include "net/handler/event_handler.h"
#include "net/socket/inet_address.h"
#include "net/socket/socket.h"
#include "net/secuity/ssl_handler.h"
#include "logger.h"

#include <cassert>
#include <cerrno>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <io.h>
#else
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace yuan::net
{
    TcpConnection::TcpConnection(const std::string ip, int port, int fd)
        : Connection()
    {
        socket_ = std::make_unique<Socket>(ip, port, false, fd);
        init();
    }

    TcpConnection::TcpConnection(Socket * sock)
        : socket_(sock), Connection()
    {
        init();
    }

    void TcpConnection::init()
    {
        connectionHandler_ = nullptr;
        eventHandler_ = nullptr;
        socket::set_none_block(socket_->get_fd(), true);
        socket::set_keep_alive(socket_->get_fd(), true);
        socket::set_no_delay(socket_->get_fd(), true);

        channel_ = std::make_unique<Channel>(socket_->get_fd());
        channel_->set_handler(this);
        channel_->enable_read();
        channel_->enable_write();

        state_ = ConnectionState::connecting;
        ssl_handler_ = nullptr;
        is_closing_ = false;
    }

    TcpConnection::~TcpConnection()
    {
        state_ = ConnectionState::closed;
        assert(channel_);
        channel_->disable_all();
        channel_->set_handler(nullptr);

        if (eventHandler_) {
            eventHandler_->close_channel(channel_.get());
            eventHandler_ = nullptr;
        }

        if (connectionHandler_) {
            connectionHandler_->on_close(this);
            connectionHandler_ = nullptr;
        }

        if (socket_) {
            LOG_WARN("connection closed, ip: {}, port: {}, fd: {}", socket_->get_address()->get_ip(), socket_->get_address()->get_port(), channel_->get_fd());
        }

        ssl_handler_.reset();
        socket_.reset();
        channel_.reset();
    }

    ConnectionState TcpConnection::get_connection_state() const
    {
        return state_;
    }

    bool TcpConnection::is_connected() const
    {
        return state_ == ConnectionState::connected;
    }

    const InetAddress &TcpConnection::get_remote_address() const
    {
        return *socket_->get_address();
    }

    void TcpConnection::write(const ::yuan::buffer::ByteBuffer & buffer)
    {
        if (buffer.empty()) {
            return;
        }

        if (state_ != ConnectionState::connected && state_ != ConnectionState::closing) {
            return;
        }

        append_output(buffer);

        channel_->enable_write();
        if (eventHandler_) {
            eventHandler_->update_channel(channel_.get());
        }
    }

    void TcpConnection::write_and_flush(const ::yuan::buffer::ByteBuffer & buffer)
    {
        if (buffer.empty() || state_ != ConnectionState::connected) {
            return;
        }

        write(buffer);
        flush();
    }

    void TcpConnection::flush()
    {
        assert(state_ == ConnectionState::connected || state_ == ConnectionState::closing);

        auto *front = output_buffer_.front();
        if (!front || front->empty()) {
            return;
        }

        const std::size_t sz = output_buffer_.size();
        for (std::size_t i = 0; i < sz;) {
            int ret;
            front = output_buffer_.front();
            if (!front || front->empty()) {
                break;
            }
            if (ssl_handler_) {
                ret = ssl_handler_->ssl_write(front->read_ptr(), front->readable_bytes());
            } else {
#ifdef _WIN32
                ret = ::send(channel_->get_fd(), front->read_ptr(), static_cast<int>(front->readable_bytes()), 0);
#else
                ret = ::send(channel_->get_fd(), front->read_ptr(), front->readable_bytes(), MSG_NOSIGNAL);
#endif
            }

            if (ret > 0) {
                if (ret >= static_cast<int>(front->readable_bytes())) {
                    output_buffer_.pop_front();
                    ++i;
                } else {
                    front->consume(static_cast<std::size_t>(ret));
                }
            } else if (ret < 0) {
#ifdef _WIN32
                const int err = WSAGetLastError();
                if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS) {
#else
                if (EAGAIN != errno && EWOULDBLOCK != errno && EINTR != errno) {
#endif
                    connectionHandler_->on_error(this);
                    close();
                    return;
                }

                channel_->enable_write();
                if (eventHandler_) {
                    eventHandler_->update_channel(channel_.get());
                }
                break;
            } else {
                break;
            }
        }

        if (output_buffer_.size() == 0) {
            if (state_ == ConnectionState::closing) {
                do_close();
            } else {
                channel_->disable_write();
                if (eventHandler_) {
                    eventHandler_->update_channel(channel_.get());
                }
            }
        }
    }

    void TcpConnection::abort()
    {
        do_close();
    }

    void TcpConnection::close()
    {
        ConnectionState lastState = state_;
        state_ = ConnectionState::closing;
        auto *front = output_buffer_.front();
        if (lastState == ConnectionState::connecting || (front && front->readable_bytes() > 0)) {
            channel_->disable_read();
            if (eventHandler_) {
                eventHandler_->update_channel(channel_.get());
            }
            return;
        }
        do_close();
    }

    Channel *TcpConnection::stream_channel() const
    {
        return channel_.get();
    }

    void TcpConnection::set_connection_handler(ConnectionHandler * handler)
    {
        this->connectionHandler_ = handler;
    }

    void TcpConnection::on_read_event()
    {
        if (state_ == ConnectionState::connecting && connectionHandler_) {
            state_ = ConnectionState::connected;
            connectionHandler_->on_connected(this);
        }

        if (ssl_handshaking_) {
            auto *ssl = ssl_handler_.get();
            if (!ssl) {
                ssl_handshaking_ = false;
                if (ssl_handshake_callback_) {
                    auto cb = std::move(ssl_handshake_callback_);
                    ssl_handshake_callback_ = nullptr;
                    cb(false);
                }
                return;
            }
            int ret = ssl->ssl_init_action();
            if (ret > 0) {
                ssl_handshaking_ = false;
                if (ssl_handshake_callback_) {
                    auto cb = std::move(ssl_handshake_callback_);
                    ssl_handshake_callback_ = nullptr;
                    cb(true);
                }
            } else if (ssl->ssl_want_read() || ssl->ssl_want_write()) {
                return;
            } else {
                ssl_handshaking_ = false;
                if (ssl_handshake_callback_) {
                    auto cb = std::move(ssl_handshake_callback_);
                    ssl_handshake_callback_ = nullptr;
                    cb(false);
                }
                close();
                return;
            }
        }

        if (!ssl_handler_) {
            bool read = false, close_flag = false;
            int bytes = 0;

            input_buffer_.clear();

            do {
                if (read && input_buffer_.writable_bytes() == 0) {
                    connectionHandler_->on_error(this);
                    close_flag = true;
                    break;
                }

#ifndef _WIN32
                bytes = ::read(channel_->get_fd(), input_buffer_.write_ptr(), input_buffer_.writable_bytes());
#else
                bytes = ::recv(channel_->get_fd(), input_buffer_.write_ptr(), static_cast<int>(input_buffer_.writable_bytes()), 0);
#endif

                if (bytes <= 0) {
                    if (bytes == 0) {
                        close_flag = true;
                    } else if (bytes == -1) {
                        if (errno != EINTR && errno != EWOULDBLOCK && errno != EAGAIN) {
#ifdef _WIN32
                            if (WSAGetLastError() == WSAEWOULDBLOCK) {
                                goto again_read;
                            }
#endif
                            LOG_ERROR("read error: {}", errno);
                            connectionHandler_->on_error(this);
                            close_flag = true;
                            break;
                        }
                    again_read:
                        channel_->enable_read();
                        if (eventHandler_) {
                            eventHandler_->update_channel(channel_.get());
                        }
                        break;
                    }
                } else {
                    read = true;
                    input_buffer_.commit(static_cast<std::size_t>(bytes));
                }
            } while (bytes > 0 && input_buffer_.writable_bytes() == 0);

            if (read && state_ == ConnectionState::connected && connectionHandler_) {
                connectionHandler_->on_read(this);
            }

            if (close_flag) {
                LOG_INFO("connection closed by peer, ip: {}, port: {}", socket_->get_address()->get_ip(), socket_->get_address()->get_port());
                abort();
            }
        } else {
            bool read = false, close_flag = false;
            int bytes = 0;

            input_buffer_.clear();

            do {
                if (read && input_buffer_.writable_bytes() == 0) {
                    connectionHandler_->on_error(this);
                    close_flag = true;
                    break;
                }

                bytes = ssl_handler_->ssl_read(input_buffer_.write_ptr(), input_buffer_.writable_bytes());

                if (bytes <= 0) {
                    if (bytes == 0) {
                        close_flag = true;
                    } else if (bytes == -1) {
                        if (errno != EINTR && errno != EWOULDBLOCK && errno != EAGAIN) {
                            LOG_ERROR("ssl read error: {}", errno);
                            connectionHandler_->on_error(this);
                            close_flag = true;
                            break;
                        }
                        channel_->enable_read();
                        if (eventHandler_) {
                            eventHandler_->update_channel(channel_.get());
                        }
                        break;
                    }
                } else {
                    read = true;
                    input_buffer_.commit(static_cast<std::size_t>(bytes));
                }
            } while (bytes > 0 && input_buffer_.writable_bytes() == 0);

            if (read && state_ == ConnectionState::connected && connectionHandler_) {
                connectionHandler_->on_read(this);
            }

            if (close_flag) {
                LOG_INFO("ssl connection closed by peer, ip: {}, port: {}", socket_->get_address()->get_ip(), socket_->get_address()->get_port());
                abort();
            }
        }
    }

    void TcpConnection::on_write_event()
    {
        if (state_ == ConnectionState::connecting && connectionHandler_) {
            state_ = ConnectionState::connected;
            connectionHandler_->on_connected(this);
        }

        if (ssl_handshaking_) {
            auto *ssl = ssl_handler_.get();
            if (!ssl) {
                ssl_handshaking_ = false;
                if (ssl_handshake_callback_) {
                    auto cb = std::move(ssl_handshake_callback_);
                    ssl_handshake_callback_ = nullptr;
                    cb(false);
                }
                return;
            }
            int ret = ssl->ssl_init_action();
            if (ret > 0) {
                ssl_handshaking_ = false;
                if (ssl_handshake_callback_) {
                    auto cb = std::move(ssl_handshake_callback_);
                    ssl_handshake_callback_ = nullptr;
                    cb(true);
                }
            } else if (ssl->ssl_want_read() || ssl->ssl_want_write()) {
                return;
            } else {
                ssl_handshaking_ = false;
                if (ssl_handshake_callback_) {
                    auto cb = std::move(ssl_handshake_callback_);
                    ssl_handshake_callback_ = nullptr;
                    cb(false);
                }
                close();
                return;
            }
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

    void TcpConnection::set_event_handler(EventHandler * eventHandler)
    {
        assert(channel_);
        eventHandler_ = eventHandler;
        if (eventHandler_) {
            eventHandler_->update_channel(channel_.get());
        }
    }

    void TcpConnection::do_close()
    {
        if (is_closing_) {
            return;
        }
        is_closing_ = true;
        if (eventHandler_) {
            eventHandler_->queue_in_loop([this]() {
                delete this;
            });
            return;
        }

        delete this;
    }

    ConnectionHandler *TcpConnection::get_connection_handler() const
    {
        return connectionHandler_;
    }

    void TcpConnection::set_ssl_handler(std::shared_ptr<SSLHandler> sslHandler)
    {
        ssl_handler_ = sslHandler;
    }
}
