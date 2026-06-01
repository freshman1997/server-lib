#include <cassert>
#include <cstring>
#ifndef _WIN32
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#else
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <windows.h>
#include <io.h>
#endif

#include "logger.h"
#include "net/connection/connection_factory.h"
#include "net/acceptor/tcp_acceptor.h"
#include "net/connection/connection.h"
#include "net/connection/stream_transport.h"
#include "net/handler/event_handler.h"
#include "net/channel/channel.h"
#include "net/socket/inet_address.h"
#include "net/socket/socket.h"
#include "net/handler/connection_handler.h"
#include "net/security/ssl_handler.h"
#include "net/security/ssl_module.h"
#include "event/event_loop.h"
#include "native_platform.h"

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

        void close_socket_fd(int fd)
        {
#ifdef _WIN32
            ::closesocket(fd);
#else
            ::close(fd);
#endif
        }
    }

    TcpAcceptor::TcpAcceptor(Socket * socket)
        : handler_(nullptr), socket_(socket)
    {
        assert(socket_);
        channel_ = std::make_unique<Channel>();
        ssl_module_ = nullptr;
        self_handler_owner_ = std::shared_ptr<SelectHandler>(this, [](SelectHandler *) {});
    }

    TcpAcceptor::~TcpAcceptor()
    {
        close();
        channel_.reset();
        self_handler_owner_.reset();
        socket_.reset();

        LOG_INFO("tcp acceptor close");
    }

    bool TcpAcceptor::listen()
    {
        return listen(128);
    }

    bool TcpAcceptor::listen(const int backlog)
    {
        assert(socket_);
        if (!socket_->listen(backlog)) {
            close();
            return false;
        }

        channel_->set_fd(socket_->get_fd());
        channel_->clear_handler();
        channel_->enable_read();

        return true;
    }

    void TcpAcceptor::close()
    {
        notify_accept_waiters(std::shared_ptr<Connection>{});
        if (channel_) {
            channel_->disable_all();
            if (handler_) {
                handler_->close_channel(ptr_of(channel_));
                handler_ = nullptr;
            }
            channel_->clear_handler();
        }
        if (conn_handler_owner_) {
            conn_handler_owner_->on_close(std::shared_ptr<Connection>{});
            conn_handler_owner_.reset();
        }
    }

    void TcpAcceptor::update_channel()
    {
        assert(channel_);
        if (handler_) {
            handler_->update_channel(ptr_of(channel_));
        }
    }

    void TcpAcceptor::on_read_event()
    {
        if (!socket_) {
            return;
        }

        while (true) {
            sockaddr_storage peer_storage{};
            memset(&peer_storage, 0, sizeof(sockaddr_storage));
            int conn_fd = socket_->accept(peer_storage);
            if (conn_fd < 0) {
#ifdef _DEBUG
                const int err = app::GetLastNativeError();
                const auto kind = app::ClassifyNativeError(err);
                if (!(app::IsNativeRetryableError(err) ||
                      kind == app::NativeError::connection_aborted)) {
                    LOG_ERROR("error connection {}", err);
                }
#endif
                break;
            }

            InetAddress peer_addr(peer_storage);

            std::shared_ptr<SSLHandler> sslHandler = nullptr;
            if (ssl_module_) {
                sslHandler = ssl_module_->create_handler(conn_fd, SSLHandler::SSLMode::acceptor_);
                if (!sslHandler) {
                    if (auto msg = ssl_module_->get_error_message()) {
                        LOG_ERROR("ssl error: {}", msg->c_str());
                    }
                    close_socket_fd(conn_fd);
                    continue;
                }
            }

            auto conn = create_stream_connection(peer_addr.get_ip(),
                                                 peer_addr.get_port(), conn_fd);

            conn->set_event_handler(handler_);
            if (conn_handler_owner_) {
                conn->set_connection_handler(conn_handler_owner_);
            }

            if (sslHandler) {
                conn->set_ssl_handler(sslHandler);
                conn->set_ssl_handshaking(true);
                if (auto stream = std::dynamic_pointer_cast<StreamTransport>(conn)) {
                    if (auto *channel = stream->stream_channel()) {
                        channel->enable_read();
                        channel->enable_write();
                    }
                }
            }

            if (auto *loop = dynamic_cast<EventLoop *>(handler_)) {
                loop->on_new_connection(conn);
            } else if (handler_) {
                handler_->on_new_connection(conn);
            }
            if (conn_handler_owner_) {
                conn_handler_owner_->on_connected(conn);
            }
            notify_accept_waiters(conn);
        }
    }

    void TcpAcceptor::on_write_event()
    {
        close();
    }

    void TcpAcceptor::set_event_handler(EventHandler * handler)
    {
        if (handler_ == handler) {
            if (handler_ && channel_) {
                handler_->update_channel(ptr_of(channel_));
            }
            return;
        }

        if (handler_ && handler_ != handler && channel_) {
            LOG_WARN("tcp acceptor event handler switched, fd: {}", channel_->get_fd());
            handler_->close_channel(ptr_of(channel_));
        }
        handler_ = handler;
        assert(channel_);
        if (handler_) {
            channel_->set_handler(std::weak_ptr<SelectHandler>(self_handler_owner_));
            handler_->update_channel(ptr_of(channel_));
        } else {
            channel_->clear_handler();
        }
    }

    void TcpAcceptor::set_connection_handler(std::shared_ptr<ConnectionHandler> connHandler)
    {
        this->conn_handler_owner_ = std::move(connHandler);
    }

    void TcpAcceptor::set_ssl_module(std::shared_ptr<SSLModule> module)
    {
        ssl_module_ = module;
    }
}
