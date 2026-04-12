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
#include "net/handler/event_handler.h"
#include "net/channel/channel.h"
#include "net/socket/socket.h"
#include "net/handler/connection_handler.h"

namespace yuan::net
{
    namespace
    {
        void close_socket_fd(int fd)
        {
    #ifdef _WIN32
            ::closesocket(fd);
    #else
            ::close(fd);
    #endif
        }
    }

    TcpAcceptor::TcpAcceptor(Socket *socket) : handler_(nullptr), socket_(socket)
    {
        assert(socket_);
        channel_ = std::make_unique<Channel>();
        ssl_module_ = nullptr;
        conn_handler_ = nullptr;
    }

    TcpAcceptor::~TcpAcceptor()
    {
        if (handler_) {
            channel_->disable_all();
            handler_->close_channel(channel_.get());
            channel_->set_handler(nullptr);
        }

        channel_.reset();
        socket_.reset();

        LOG_INFO("tcp acceptor close");
    }

    bool TcpAcceptor::listen()
    {
        assert(socket_);
        if (!socket_->listen()) {
            close();
            return false;
        }

        channel_->set_fd(socket_->get_fd());
        channel_->set_handler(this);
        channel_->enable_read();

        return true;
    }

    void TcpAcceptor::close()
    {
        if (handler_) {
            handler_->queue_in_loop([this]() {
                delete this;
            });
            return;
        }

        delete this;
    }

    void TcpAcceptor::update_channel()
    {
        assert(channel_);
        if (handler_) {
            handler_->update_channel(channel_.get());
        }
    }

    void TcpAcceptor::on_read_event()
    {
        assert(socket_);

        while (true) {
            sockaddr_in peer_addr{};
            memset(&peer_addr, 0, sizeof(sockaddr_in));
            int conn_fd = socket_->accept(peer_addr);
            if (conn_fd < 0) {
            #ifdef _DEBUG
                if (errno != EAGAIN && errno != ECONNABORTED && errno != EPROTO && errno != EINTR) {
                    LOG_ERROR("error connection {}", errno);
                }
            #endif
                break;
            }

            std::shared_ptr<SSLHandler> sslHandler = nullptr;
            if (ssl_module_) {
                sslHandler = ssl_module_->create_handler(conn_fd, SSLHandler::SSLMode::acceptor_);
                if (!sslHandler || sslHandler->ssl_init_action() <= 0) {
                    if (auto msg = ssl_module_->get_error_message()) {
                        LOG_ERROR("ssl error: {}", msg->c_str());
                    }
                    close_socket_fd(conn_fd);
                    continue;
                }
            }

            Connection *conn = create_stream_connection(::inet_ntoa(peer_addr.sin_addr),
                                                        ntohs(peer_addr.sin_port), conn_fd);

            conn->set_event_handler(handler_);
            conn->set_connection_handler(conn_handler_);

            if (sslHandler) {
                conn->set_ssl_handler(sslHandler);
            }

            handler_->on_new_connection(conn);
        }
    }

    void TcpAcceptor::on_write_event()
    {
        close();
    }

    void TcpAcceptor::set_event_handler(EventHandler *handler)
    {
        handler_ = handler;
        assert(channel_);
        if (handler_) {
            handler_->update_channel(channel_.get());
        }
    }

    void TcpAcceptor::set_connection_handler(ConnectionHandler *connHandler)
    {
        this->conn_handler_ = connHandler;
    }

    void TcpAcceptor::set_ssl_module(std::shared_ptr<SSLModule> module)
    {
        ssl_module_ = module;
    }
}
