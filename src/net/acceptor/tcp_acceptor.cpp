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
    TcpAcceptor::TcpAcceptor(Socket *socket) : handler_(nullptr), channel_(nullptr), socket_(socket)
    {}

    bool TcpAcceptor::listen()
    {
        assert(socket_);
        if (!socket_->listen()) {
            on_write_event();
            return false;
        }

        channel_ = new Channel(socket_->get_fd());
        channel_->set_handler(this);
        channel_->enable_read();
        return true;
    }

    void TcpAcceptor::on_close()
    {
        ::close(channel_->get_fd());
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
            std::shared_ptr<InetAddress> remote_addr = std::make_shared<InetAddress>(::inet_ntoa(peer_addr.sin_addr), ntohs(peer_addr.sin_port));
            std::shared_ptr<Channel> newChannel = std::make_shared<Channel>(conn_fd);
            Connection *conn = new TcpConnection(remote_addr, newChannel);
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
    }

    const Socket * TcpAcceptor::get_socket() const
    {
        return socket_;
    }
}