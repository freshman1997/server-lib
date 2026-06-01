#include "net/connection/tcp_connection.h"
#include "net/channel/channel.h"
#include "net/connection/connection.h"
#include "net/handler/connection_handler.h"
#include "net/socket/socket_ops.h"
#include "net/handler/event_handler.h"
#include "net/socket/inet_address.h"
#include "net/socket/socket.h"
#include "net/security/ssl_handler.h"
#include "native_platform.h"
#include "logger.h"

#include <cassert>
#include <cerrno>
#ifdef _WIN32
#include <ws2tcpip.h>
#include <winsock2.h>
#include <windows.h>
#include <io.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace yuan::net
{
    namespace
    {
        bool is_connect_completed(int fd)
        {
            int so_error = 0;
#ifdef _WIN32
            int len = static_cast<int>(sizeof(so_error));
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&so_error), &len) != 0) {
                return false;
            }
            return so_error == 0 || so_error == WSAEISCONN;
#else
            socklen_t len = static_cast<socklen_t>(sizeof(so_error));
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) != 0) {
                return false;
            }
            return so_error == 0 || so_error == EISCONN;
#endif
        }

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

        bool is_transient_send_error(const int err) noexcept
        {
            return app::IsNativeRetryableError(err);
        }

        bool is_transient_errno(const int err) noexcept
        {
            return app::IsNativeRetryableError(err);
        }

        int socket_error_code(const int fd) noexcept
        {
            if (fd < 0) {
#ifdef _WIN32
                return WSAENOTSOCK;
#else
                return EBADF;
#endif
            }

            int so_error = 0;
#ifdef _WIN32
            int len = static_cast<int>(sizeof(so_error));
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&so_error), &len) != 0) {
                return app::GetLastNativeError();
            }
#else
            socklen_t len = static_cast<socklen_t>(sizeof(so_error));
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) != 0) {
                return app::GetLastNativeError();
            }
#endif
            return so_error;
        }
    }

    TcpConnection::TcpConnection(const std::string ip, int port, int fd)
        : Connection()
    {
        socket_ = std::make_unique<Socket>(ip, port, false, fd);
        init();
        state_ = ConnectionState::connected;
        channel_->disable_write();
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

        local_address_ = socket_->get_local_address();

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
        const bool had_handler = channel_->has_handler();
        channel_->disable_all();

        if (eventHandler_ && !cleanup_done_ && had_handler) {
            eventHandler_->close_channel(ptr_of(channel_));
            eventHandler_ = nullptr;
        }
        channel_->clear_handler();

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
        return local_address_;
    }

    void TcpConnection::write(const ::yuan::buffer::ByteBuffer & buffer)
    {
        if (buffer.empty()) {
            return;
        }

        if (state_ != ConnectionState::connected || is_closing_ || output_shutdown_) {
            LOG_WARN("write dropped: state={}, output_shutdown={}, fd={}", static_cast<int>(state_), output_shutdown_, channel_->get_fd());
            return;
        }

        append_output(buffer);
        if (output_limit_exceeded()) {
            do_close();
            return;
        }

        channel_->enable_write();
        if (eventHandler_) {
            eventHandler_->update_channel(ptr_of(channel_));
        }
    }

    void TcpConnection::write_owned(::yuan::buffer::ByteBuffer buffer)
    {
        if (buffer.empty()) {
            return;
        }

        if (state_ != ConnectionState::connected || is_closing_ || output_shutdown_) {
            return;
        }

        bool overflow = false;
        {
            std::lock_guard<std::mutex> lock(output_buffer_mutex_);
            const auto bytes = buffer.readable_bytes();
            const auto limit = max_output_buffer_size();
            if (limit != 0 && (bytes > limit || output_buffer_.readable_bytes() > limit - bytes)) {
                output_limit_exceeded_.store(true, std::memory_order_release);
                overflow = true;
            } else {
                output_buffer_.push_back(std::make_unique<::yuan::buffer::ByteBuffer>(std::move(buffer)));
            }
        }
        if (overflow) {
            do_close();
            return;
        }

        channel_->enable_write();
        if (eventHandler_) {
            eventHandler_->update_channel(ptr_of(channel_));
        }
    }

    void TcpConnection::write_and_flush(const ::yuan::buffer::ByteBuffer & buffer)
    {
        if (buffer.empty() || state_ != ConnectionState::connected || is_closing_) {
            if (!buffer.empty()) {
                LOG_WARN("write_and_flush dropped: state={}, fd={}", static_cast<int>(state_), channel_->get_fd());
            }
            return;
        }

        write(buffer);
        flush();
    }

    void TcpConnection::write_owned_and_flush(::yuan::buffer::ByteBuffer buffer)
    {
        if (buffer.empty() || state_ != ConnectionState::connected || is_closing_) {
            return;
        }

        write_owned(std::move(buffer));
        flush();
    }

    void TcpConnection::write_raw_and_flush(std::string_view data)
    {
        if (data.empty() || state_ != ConnectionState::connected || is_closing_ || output_shutdown_) {
            return;
        }

        if (ssl_handler_ || output_buffer_.front()) {
            append_output(data);
            if (output_limit_exceeded()) {
                do_close();
                return;
            }
            flush();
            return;
        }

        int ret = 0;
        int write_error = 0;
#ifdef _WIN32
        ret = ::send(channel_->get_fd(), data.data(), static_cast<int>(data.size()), 0);
        write_error = ret < 0 ? app::GetLastNativeError() : 0;
#else
        ret = ::send(channel_->get_fd(), data.data(), data.size(), MSG_NOSIGNAL);
        write_error = ret < 0 ? app::GetLastNativeError() : 0;
#endif

        if (ret == static_cast<int>(data.size())) {
            finish_output_drained();
            return;
        }

        if (ret > 0) {
            data.remove_prefix(static_cast<std::size_t>(ret));
        } else if (ret < 0) {
            bool transient_write_error = is_transient_send_error(write_error);
            if (transient_write_error) {
                const int socket_error = socket_error_code(channel_->get_fd());
                if (socket_error != 0) {
                    write_error = socket_error;
                    transient_write_error = false;
                }
            }

            if (!transient_write_error) {
                if (connectionHandlerOwner_) {
                    notify_event_waiters(ConnectionEvent::error);
                    connectionHandlerOwner_->on_error(shared_from_this());
                }
                do_close();
                return;
            }
        }

        append_output(data);
        if (output_limit_exceeded()) {
            do_close();
            return;
        }
        channel_->enable_write();
        if (eventHandler_) {
            eventHandler_->update_channel(ptr_of(channel_));
        }
    }

    void TcpConnection::flush()
    {
        if (state_ == ConnectionState::closed || is_closing_) {
            return;
        }

        assert(state_ == ConnectionState::connected || state_ == ConnectionState::closing);

        if (finish_output_drained()) {
            return;
        }

        auto *front = output_buffer_.front();
        if (!front || front->empty()) {
            return;
        }

        LOG_TRACE("flush: fd={}, output_chunks={}, first_chunk_readable={}", channel_->get_fd(), output_buffer_.size(), front->readable_bytes());

        const std::size_t sz = output_buffer_.size();
        for (std::size_t i = 0; i < sz;) {
            int ret;
            int write_error = 0;
            bool transient_write_error = false;
            front = output_buffer_.front();
            if (!front || front->empty()) {
                break;
            }
            if (ssl_handler_) {
                ret = ssl_handler_->ssl_write(front->read_ptr(), front->readable_bytes());
                write_error = ret < 0 ? app::GetLastNativeError() : 0;
                transient_write_error = ret < 0 && is_transient_errno(write_error);
            } else {
#ifdef _WIN32
                ret = ::send(channel_->get_fd(), front->read_ptr(), static_cast<int>(front->readable_bytes()), 0);
                write_error = ret < 0 ? app::GetLastNativeError() : 0;
#else
                ret = ::send(channel_->get_fd(), front->read_ptr(), front->readable_bytes(), MSG_NOSIGNAL);
                write_error = ret < 0 ? app::GetLastNativeError() : 0;
#endif
                transient_write_error = ret < 0 && is_transient_send_error(write_error);
            }

            if (ret < 0 && transient_write_error && !ssl_handler_) {
                const int socket_error = socket_error_code(channel_->get_fd());
                if (socket_error != 0) {
                    write_error = socket_error;
                    transient_write_error = false;
                }
            }

            if (ret < 0 && !transient_write_error) {
                if (ssl_handler_) {
                    LOG_DEBUG("flush ssl write failed: fd={}, ret={}, readable={}, errno={}", channel_->get_fd(), ret, front->readable_bytes(), write_error);
                } else {
#ifdef _WIN32
                    LOG_DEBUG("flush send failed: fd={}, ret={}, readable={}, wsa_error={}", channel_->get_fd(), ret, front->readable_bytes(), write_error);
#else
                    LOG_DEBUG("flush send failed: fd={}, ret={}, readable={}, errno={}", channel_->get_fd(), ret, front->readable_bytes(), write_error);
#endif
                }
            } else {
                LOG_TRACE("flush send: fd={}, ret={}, readable={}", channel_->get_fd(), ret, front->readable_bytes());
            }

            if (ret > 0) {
                if (ret >= static_cast<int>(front->readable_bytes())) {
                    output_buffer_.pop_front();
                    ++i;
                } else {
                    front->consume(static_cast<std::size_t>(ret));
                }
            } else if (ret < 0) {
                if (!transient_write_error) {
                    if (connectionHandlerOwner_) {
                        notify_event_waiters(ConnectionEvent::error);
                        connectionHandlerOwner_->on_error(shared_from_this());
                    }
                    do_close();
                    return;
                }

#ifdef _WIN32
                if (!ssl_handler_ && app::ClassifyNativeError(write_error) == app::NativeError::interrupted) {
                    continue;
                }
#else
                if (app::ClassifyNativeError(write_error) == app::NativeError::interrupted) {
                    continue;
                }
#endif

                if (ssl_handler_) {
                    update_ssl_handshake_interest(*ssl_handler_);
                } else {
                    channel_->enable_write();
                }
                if (eventHandler_) {
                    eventHandler_->update_channel(ptr_of(channel_));
                }
                break;
            } else {
                break;
            }
        }

        finish_output_drained();
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
        if (state_ == ConnectionState::closed || is_closing_) {
            return;
        }

        ConnectionState lastState = state_;
        state_ = ConnectionState::closing;
        auto *front = output_buffer_.front();
        if (lastState == ConnectionState::connecting) {
            do_close();
            return;
        }
        if (front && front->readable_bytes() > 0) {
            channel_->disable_read();
            channel_->enable_write();
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

        if (state_ == ConnectionState::connecting) {
            if (!is_connect_completed(channel_->get_fd())) {
                notify_event_waiters(ConnectionEvent::error);
                if (handler) {
                    handler->on_error(shared_from_this());
                }
                close();
                return;
            }
            state_ = ConnectionState::connected;
            local_address_ = socket_->get_local_address();
            notify_event_waiters(ConnectionEvent::connected);
            if (handler) {
                handler->on_connected(shared_from_this());
            }
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
                update_ssl_handshake_interest(*ssl);
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

            for (;;) {
                if (read && input_buffer_.writable_bytes() == 0) {
                    if (drain_grow_input_buffer()) {
                        continue;
                    }
                    break;
                }

                if (input_buffer_.writable_bytes() == 0) {
                    if (!grow_input_buffer()) {
                        if (handler) {
                            handler->on_error(shared_from_this());
                        }
                        close_flag = true;
                        break;
                    }
                    continue;
                }

#ifndef _WIN32
                bytes = ::read(channel_->get_fd(), input_buffer_.write_ptr(), input_buffer_.writable_bytes());
#else
                bytes = ::recv(channel_->get_fd(), input_buffer_.write_ptr(), static_cast<int>(input_buffer_.writable_bytes()), 0);
#endif

                if (bytes <= 0) {
                    if (bytes == 0) {
                        close_flag = true;
                        break;
#ifdef _WIN32
                    } else if (bytes == SOCKET_ERROR) {
                        const int err = app::GetLastNativeError();
                        if (!app::IsNativeRetryableError(err)) {
                            LOG_ERROR("read error: {}", err);
                            notify_event_waiters(ConnectionEvent::error);
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
#else
                    } else if (bytes == -1) {
                        const int err = app::GetLastNativeError();
                        if (!app::IsNativeRetryableError(err)) {
                            LOG_ERROR("read error: {}", err);
                            notify_event_waiters(ConnectionEvent::error);
                            if (handler) {
                                handler->on_error(shared_from_this());
                            }
                            close_flag = true;
                            break;
                        }
                        if (app::ClassifyNativeError(err) == app::NativeError::interrupted) {
                            continue;
                        }
                        channel_->enable_read();
                        if (eventHandler_) {
                            eventHandler_->update_channel(ptr_of(channel_));
                        }
                        break;
#endif
                    }
                } else {
                    read = true;
                    input_buffer_.commit(static_cast<std::size_t>(bytes));
                }
            }

            if (read && state_ == ConnectionState::connected) {
                notify_event_waiters(ConnectionEvent::readable);
                if (handler) {
                    handler->on_read(shared_from_this());
                }
            }

            if (close_flag) {
                if (state_ == ConnectionState::closed || is_closing_) {
                    return;
                }
                LOG_INFO("connection closed by peer, ip: {}, port: {}", socket_->get_address()->get_ip(), socket_->get_address()->get_port());
                input_shutdown_ = true;
                channel_->disable_read();
                if (eventHandler_) {
                    eventHandler_->update_channel(ptr_of(channel_));
                }
                notify_event_waiters(ConnectionEvent::input_shutdown);
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

            for (;;) {
                if (read && input_buffer_.writable_bytes() == 0) {
                    if (drain_grow_input_buffer()) {
                        continue;
                    }
                    break;
                }

                if (input_buffer_.writable_bytes() == 0) {
                    if (!grow_input_buffer()) {
                        if (handler) {
                            handler->on_error(shared_from_this());
                        }
                        close_flag = true;
                        break;
                    }
                    continue;
                }

                bytes = ssl_handler_->ssl_read(input_buffer_.write_ptr(), input_buffer_.writable_bytes());

                if (bytes <= 0) {
                    if (bytes == 0) {
                        close_flag = true;
                        break;
                    } else if (bytes == -1) {
                        const int err = app::GetLastNativeError();
                        if (!app::IsNativeRetryableError(err)) {
                            LOG_ERROR("ssl read error: {}", err);
                            notify_event_waiters(ConnectionEvent::error);
                            if (handler) {
                                handler->on_error(shared_from_this());
                            }
                            close_flag = true;
                            break;
                        }
                        if (app::ClassifyNativeError(err) == app::NativeError::interrupted) {
                            continue;
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
            }

            if (read && state_ == ConnectionState::connected) {
                notify_event_waiters(ConnectionEvent::readable);
                if (handler) {
                    handler->on_read(shared_from_this());
                }
            }

            if (close_flag) {
                if (state_ == ConnectionState::closed || is_closing_) {
                    return;
                }
                LOG_INFO("ssl connection closed by peer, ip: {}, port: {}", socket_->get_address()->get_ip(), socket_->get_address()->get_port());
                input_shutdown_ = true;
                channel_->disable_read();
                if (eventHandler_) {
                    eventHandler_->update_channel(ptr_of(channel_));
                }
                notify_event_waiters(ConnectionEvent::input_shutdown);
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

        if (state_ == ConnectionState::connecting) {
            if (!is_connect_completed(channel_->get_fd())) {
                notify_event_waiters(ConnectionEvent::error);
                if (handler) {
                    handler->on_error(shared_from_this());
                }
                close();
                return;
            }
            state_ = ConnectionState::connected;
            local_address_ = socket_->get_local_address();
            notify_event_waiters(ConnectionEvent::connected);
            if (handler) {
                handler->on_connected(shared_from_this());
            }
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
                update_ssl_handshake_interest(*ssl);
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

        if (state_ == ConnectionState::connected || state_ == ConnectionState::closing) {
            flush();
            if (output_readable_bytes() == 0) {
                notify_event_waiters(ConnectionEvent::writable);
                if (handler) {
                    handler->on_write(shared_from_this());
                }
                if (output_readable_bytes() > 0) {
                    flush();
                }
                if (state_ == ConnectionState::closing && output_readable_bytes() == 0) {
                    do_close();
                }
            }
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
            state_ = ConnectionState::closed;
            output_buffer_.clear();
            if (channel_) {
                channel_->disable_all();
            }
            return;
        }
        is_closing_ = true;
        state_ = ConnectionState::closed;
        input_shutdown_ = true;
        output_shutdown_ = true;
        pending_output_shutdown_ = false;
        output_buffer_.clear();
        if (channel_) {
            channel_->disable_all();
        }

        [[maybe_unused]] auto handler_owner = std::move(connectionHandlerOwner_);
        auto *handler = ptr_of(handler_owner);

        if (!close_notified_) {
            close_notified_ = true;
            notify_event_waiters(ConnectionEvent::closed);
            if (handler) {
                handler->on_close(shared_from_this());
            }
            clear_event_waiters();
        }

        if (socket_) {
            socket_->close();
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

    bool TcpConnection::finish_output_drained()
    {
        while (auto *front = output_buffer_.front()) {
            if (!front->empty()) {
                break;
            }
            output_buffer_.pop_front();
        }

        if (output_buffer_.size() != 0) {
            return false;
        }

        if (pending_output_shutdown_ && !output_shutdown_) {
            output_shutdown_ = socket_ && socket_->shutdown_write();
            pending_output_shutdown_ = false;
            if (output_shutdown_ && input_shutdown_) {
                do_close();
                return true;
            }
        }

        if (state_ == ConnectionState::closing) {
            do_close();
            return true;
        }

        if (channel_) {
            const bool had_write_interest = (channel_->get_events() & Channel::WRITE_EVENT) != 0;
            channel_->disable_write();
            if (had_write_interest && eventHandler_) {
                eventHandler_->update_channel(ptr_of(channel_));
            }
        }
        return true;
    }

    void TcpConnection::update_ssl_handshake_interest(SSLHandler &ssl)
    {
        if (!channel_) {
            return;
        }

        if (ssl.ssl_want_read()) {
            channel_->enable_read();
        }
        if (ssl.ssl_want_write()) {
            channel_->enable_write();
        } else {
            channel_->disable_write();
        }

        if (eventHandler_) {
            eventHandler_->update_channel(ptr_of(channel_));
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
