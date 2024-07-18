#include <cassert>
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
#include <iostream>

#include "net/base/acceptor/tcp_acceptor.h"
#include "net/base/connection/connection.h"
#include "net/base/connection/tcp_connection.h"
#include "net/base/handler/event_handler.h"
#include "net/base/channel/channel.h"
#include "net/base/socket/socket.h"
#include "net/base/handler/connection_handler.h"

namespace net
{
    TcpAcceptor::TcpAcceptor(Socket *socket) : handler_(nullptr), socket_(socket)
    {
        assert(socket_);
        channel_ = new Channel;
    }

    TcpAcceptor::~TcpAcceptor()
    {
        if (handler_) {
            channel_->disable_all();
            handler_->close_channel(channel_);
            channel_->set_handler(nullptr);
            channel_ = nullptr;
        }
        
        if (socket_) {
            delete socket_;
            socket_ = nullptr;
        }

        std::cout << "tcp acceptor close\n";
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
        delete this;
    }

    void TcpAcceptor::on_read_event()
    {
        assert(socket_);

        struct sockaddr_in peer_addr;
        int conn_fd = socket_->accept(peer_addr);
        if (conn_fd < 0) {
            std::cout << "error connection " << std::endl;
            delete this;
        } else {
            Connection *conn = new TcpConnection(::inet_ntoa(peer_addr.sin_addr), 
                        ntohs(peer_addr.sin_port), conn_fd);
                        
            conn->set_event_handler(handler_);
            conn->set_connection_handler(conn_handler_);

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
        handler_->update_channel(channel_);
    }

    void TcpAcceptor::set_connection_handler(ConnectionHandler *connHandler)
    {
        this->conn_handler_ = connHandler;
    }
}