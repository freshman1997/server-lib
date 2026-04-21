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
    namespace
    {
        template<typename T>
        T *ptr_of(std::unique_ptr<T> &owner)
        {
            return owner ? &*owner : nullptr;
        }

        template<typename T>
        const T *ptr_of(const std::unique_ptr<T> &owner)
        {
            return owner ? &*owner : nullptr;
        }

        template<typename T>
        T *ptr_of(std::shared_ptr<T> &owner)
        {
            return owner ? &*owner : nullptr;
        }

        template<typename T>
        T *ptr_of(const std::shared_ptr<T> &owner)
        {
            return owner ? const_cast<T *>(&*owner) : nullptr;
        }
    }

    TcpConnection::TcpConnection(const std::string ip, int port, int fd)
        : Connection()
    {
        socket_ = std::make_unique<Socket>(ip, port, false, fd);
        init();
        state_ = ConnectionState::connected;
    }

    TcpConnection::TcpConnection(Socket * sock)
        : socket_(sock), Connection()
    {
        init();
    }

    void TcpConnection::init()
    {
        connectionHandlerOwner_.reset();
        eventHandler_ = nullptr;
        socket::set_none_block(socket_->get_fd(), true);
        socket::set_keep_alive(socket_->get_fd(), true);
        socket::set_no_delay(socket_->get_fd(), true);

        channel_ = std::make_unique<Channel>(socket_->get_fd());
        channel_->clear_handler();
        channel_->enable_read();
        channel_->enable_write();

        state_ = ConnectionState::connecting;
        ssl_handler_ = nullptr;
        is_closing_ = false;
        input_shutdown_ = false;
        output_shutdown_ = false;
        pending_output_shutdown_ = false;
    }

    TcpConnection::~TcpConnection()
    {
        state_ = ConnectionState::closed;
        assert(channel_);
        channel_->disable_all();
        channel_->clear_handler();

        if (eventHandler_ && !cleanup_done_) {
            eventHandler_->close_channel(ptr_of(channel_));
            eventHandler_ = nullptr;
        }

        if (connectionHandlerOwner_ && !close_notified_) {
            LOG_WARN("tcp connection destroyed without close notification, ip: {}, port: {}, fd: {}",
                     socket_ ? socket_->get_address()->get_ip() : std::string{},
                     socket_ ? socket_->get_address()->get_port() : 0,
                     channel_->get_fd());
        }
        connectionHandlerOwner_.reset();

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

    const InetAddress &TcpConnection::get_local_address() const
    {
        return *socket_->get_address();
    }

    void TcpConnection::write(const ::yuan::buffer::ByteBuffer & buffer)
    {
        if (buffer.empty()) {
            return;
        }

        if ((state_ != ConnectionState::connected && state_ != ConnectionState::closing) || output_shutdown_) {
            return;
        }

        append_output(buffer);

        channel_->enable_write();
        if (eventHandler_) {
            eventHandler_->update_channel(ptr_of(channel_));
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
                    if (connectionHandlerOwner_) {
                        connectionHandlerOwner_->on_error(shared_from_this());
                    }
                    close();
                    return;
                }

                channel_->enable_write();
                if (eventHandler_) {
                    eventHandler_->update_channel(ptr_of(channel_));
                }
                break;
            } else {
                break;
            }
        }

        if (output_buffer_.size() == 0) {
            if (pending_output_shutdown_ && !output_shutdown_) {
                output_shutdown_ = socket_ && socket_->shutdown_write();
                pending_output_shutdown_ = false;
                if (output_shutdown_ && input_shutdown_) {
                    do_close();
                    return;
                }
            }
            if (state_ == ConnectionState::closing) {
                do_close();
            } else {
                channel_->disable_write();
                if (eventHandler_) {
                    eventHandler_->update_channel(ptr_of(channel_));
                }
            }
        }
    }

    void TcpConnection::abort()
    {
        do_close();
    }

    bool TcpConnection::shutdown_write()
    {
        if (!socket_ || output_shutdown_) {
            return false;
        }

        auto *front = output_buffer_.front();
        if (front && front->readable_bytes() > 0) {
            pending_output_shutdown_ = true;
            channel_->enable_write();
            if (eventHandler_) {
                eventHandler_->update_channel(ptr_of(channel_));
            }
            return true;
        }

        output_shutdown_ = socket_->shutdown_write();
        pending_output_shutdown_ = false;
        if (output_shutdown_ && input_shutdown_) {
            do_close();
        }
        return output_shutdown_;
    }

    bool TcpConnection::input_shutdown() const
    {
        return input_shutdown_;
    }

    void TcpConnection::close()
    {
        ConnectionState lastState = state_;
        state_ = ConnectionState::closing;
        auto *front = output_buffer_.front();
        if (lastState == ConnectionState::connecting || (front && front->readable_bytes() > 0)) {
            channel_->disable_read();
            if (eventHandler_) {
                eventHandler_->update_channel(ptr_of(channel_));
            }
            return;
        }
        do_close();
    }

    Channel *TcpConnection::stream_channel()
    {
        return ptr_of(channel_);
    }

    const Channel *TcpConnection::stream_channel() const
    {
        return ptr_of(channel_);
    }

    void TcpConnection::set_connection_handler(std::shared_ptr<ConnectionHandler> handler)
    {
        connectionHandlerOwner_ = std::move(handler);
    }

    void TcpConnection::on_read_event()
    {
        [[maybe_unused]] auto handler_owner = connectionHandlerOwner_;
        auto *handler = ptr_of(handler_owner);

        if (state_ == ConnectionState::connecting && handler) {
            state_ = ConnectionState::connected;
            handler->on_connected(shared_from_this());
        }

        if (ssl_handshaking_) {
            auto *ssl = ptr_of(ssl_handler_);
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
                    if (handler) {
                        handler->on_error(shared_from_this());
                    }
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
                            if (handler) {
                                handler->on_error(shared_from_this());
                            }
                            close_flag = true;
                            break;
                        }
                    again_read:
                        channel_->enable_read();
                        if (eventHandler_) {
                            eventHandler_->update_channel(ptr_of(channel_));
                        }
                        break;
                    }
                } else {
                    read = true;
                    input_buffer_.commit(static_cast<std::size_t>(bytes));
                }
            } while (bytes > 0 && input_buffer_.writable_bytes() == 0);

            if (read && state_ == ConnectionState::connected && handler) {
                handler->on_read(shared_from_this());
            }

            if (close_flag) {
                LOG_INFO("connection closed by peer, ip: {}, port: {}", socket_->get_address()->get_ip(), socket_->get_address()->get_port());
                input_shutdown_ = true;
                channel_->disable_read();
                if (eventHandler_) {
                    eventHandler_->update_channel(ptr_of(channel_));
                }
                if (handler) {
                    handler->on_input_shutdown(shared_from_this());
                }
                auto *front = output_buffer_.front();
                const bool has_pending_output = front && front->readable_bytes() > 0;
                if (output_shutdown_) {
                    do_close();
                } else if (state_ == ConnectionState::closing && !has_pending_output) {
                    do_close();
                }
            }
        } else {
            bool read = false, close_flag = false;
            int bytes = 0;

            input_buffer_.clear();

            do {
                if (read && input_buffer_.writable_bytes() == 0) {
                    if (handler) {
                        handler->on_error(shared_from_this());
                    }
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
                            if (handler) {
                                handler->on_error(shared_from_this());
                            }
                            close_flag = true;
                            break;
                        }
                        channel_->enable_read();
                        if (eventHandler_) {
                            eventHandler_->update_channel(ptr_of(channel_));
                        }
                        break;
                    }
                } else {
                    read = true;
                    input_buffer_.commit(static_cast<std::size_t>(bytes));
                }
            } while (bytes > 0 && input_buffer_.writable_bytes() == 0);

            if (read && state_ == ConnectionState::connected && handler) {
                handler->on_read(shared_from_this());
            }

            if (close_flag) {
                LOG_INFO("ssl connection closed by peer, ip: {}, port: {}", socket_->get_address()->get_ip(), socket_->get_address()->get_port());
                input_shutdown_ = true;
                channel_->disable_read();
                if (eventHandler_) {
                    eventHandler_->update_channel(ptr_of(channel_));
                }
                if (handler) {
                    handler->on_input_shutdown(shared_from_this());
                }
                auto *front = output_buffer_.front();
                const bool has_pending_output = front && front->readable_bytes() > 0;
                if (output_shutdown_) {
                    do_close();
                } else if (state_ == ConnectionState::closing && !has_pending_output) {
                    do_close();
                }
            }
        }
    }

    void TcpConnection::on_write_event()
    {
        [[maybe_unused]] auto handler_owner = connectionHandlerOwner_;
        auto *handler = ptr_of(handler_owner);

        if (state_ == ConnectionState::connecting && handler) {
            state_ = ConnectionState::connected;
            handler->on_connected(shared_from_this());
        }

        if (ssl_handshaking_) {
            auto *ssl = ptr_of(ssl_handler_);
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

        if ((state_ == ConnectionState::connected || state_ == ConnectionState::closing) && handler) {
            handler->on_write(shared_from_this());
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
        if (eventHandler_ == eventHandler) {
            if (eventHandler_) {
                eventHandler_->update_channel(ptr_of(channel_));
            }
            return;
        }

        if (eventHandler_ && eventHandler_ != eventHandler) {
            LOG_WARN("tcp connection event handler switched, fd: {}", channel_->get_fd());
            eventHandler_->close_channel(ptr_of(channel_));
        }
        eventHandler_ = eventHandler;
        if (eventHandler_) {
            auto self = std::static_pointer_cast<SelectHandler>(shared_from_this());
            channel_->set_handler(std::weak_ptr<SelectHandler>(self));
            cleanup_done_ = false;
        } else {
            channel_->clear_handler();
        }
        if (eventHandler_) {
            eventHandler_->update_channel(ptr_of(channel_));
        }
    }

    void TcpConnection::do_close()
    {
        if (is_closing_) {
            return;
        }
        is_closing_ = true;

        [[maybe_unused]] auto handler_owner = std::move(connectionHandlerOwner_);
        auto *handler = ptr_of(handler_owner);

        if (handler && !close_notified_) {
            close_notified_ = true;
            handler->on_close(shared_from_this());
        }

        if (eventHandler_ && channel_) {
            auto self = std::static_pointer_cast<TcpConnection>(shared_from_this());
            auto *event_handler = eventHandler_;
            event_handler->queue_in_loop([self, event_handler]() {
                if (self->channel_ && !self->cleanup_done_) {
                    event_handler->close_channel(ptr_of(self->channel_));
                    self->cleanup_done_ = true;
                }
            });
            return;
        }
    }

    ConnectionHandler *TcpConnection::get_connection_handler() const
    {
        return ptr_of(connectionHandlerOwner_);
    }

    void TcpConnection::set_ssl_handler(std::shared_ptr<SSLHandler> sslHandler)
    {
        ssl_handler_ = sslHandler;
    }
}
