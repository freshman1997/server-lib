#include <arpa/inet.h>
#include <cassert>
#include <memory>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <iostream>

#include "net/acceptor/tcp_acceptor.h"
#include "net/connection/connection.h"
#include "net/connection/tcp_connection.h"
#include "net/handler/event_handler.h"
#include "net/channel/channel.h"
#include "net/socket/inet_address.h"
#include "net/socket/socket.h"

namespace net
{
    TcpAcceptor::TcpAcceptor(Socket *socket) : handler_(nullptr), socket_(socket)
    {
        assert(socket_);
    }

    TcpAcceptor::~TcpAcceptor()
    {
        delete socket_;
        std::cout << "acceptor close\n";
    }

    bool TcpAcceptor::listen()
    {
        assert(socket_);
        if (!socket_->listen()) {
            on_write_event();
            return false;
        }

        channel_.set_fd(socket_->get_fd());
        channel_.set_handler(this);
        channel_.enable_read();
        channel_.enable_write();

        return true;
    }

    void TcpAcceptor::on_close()
    {
        ::close(channel_.get_fd());
    }

    void TcpAcceptor::on_read_event()
    {
        assert(socket_);

        struct sockaddr_in peer_addr;
        int conn_fd = socket_->accept(peer_addr);
        if (conn_fd < 0) {
            std::cout << "error connection " << std::endl;
            handler_->on_quit(this);
            delete this;
        } else {
            Connection *conn = new TcpConnection(::inet_ntoa(peer_addr.sin_addr), 
                        ntohs(peer_addr.sin_port), conn_fd);
            conn->set_event_handler(handler_);

            handler_->on_new_connection(conn, this);
        }
    }

    void TcpAcceptor::on_write_event()
    {
        if (handler_) {
            handler_->on_quit(this);
        }

        on_close();
    }

    void TcpAcceptor::set_event_handler(EventHandler *handler)
    {
        handler_ = handler;
        handler_->update_event(&channel_);
    }

    const Socket * TcpAcceptor::get_socket() const
    {
        return socket_;
    }
}