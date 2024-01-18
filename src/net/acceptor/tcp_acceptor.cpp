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
#include "net/handler/accept_handler.h"
#include "net/channel/channel.h"
#include "net/socket/inet_address.h"
#include "net/socket/socket.h"
#include "net/socket/socket_ops.h"

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
        ::close(get_fd());
    }

    void TcpAcceptor::set_handler(AcceptHandler *handler)
    {
        handler_ = handler;
    }

    void TcpAcceptor::on_new_connection(Connection *conn)
    {

    }

    void TcpAcceptor::on_read_event()
    {
        assert(socket_);
        // TODO accept
        struct sockaddr_in peer_addr;
        int conn_fd = socket_->accept(peer_addr);
        if (conn_fd < 0) {
            //on_write_event();
            std::cout << "error connection " << std::endl;
            return;
        }

        std::cout << "connection fd " << conn_fd << std::endl;
        if (!handler_->is_unique(conn_fd)) {
            std::cout << "===> duplicate connection <===" << conn_fd << std::endl;
            return;
        }

        std::shared_ptr<InetAddress> remote_addr = std::make_shared<InetAddress>(::inet_ntoa(peer_addr.sin_addr), ntohs(peer_addr.sin_port));
        std::shared_ptr<InetAddress> local_addr(socket_->get_address());
        std::shared_ptr<Channel> newChannel = std::make_shared<Channel>(conn_fd);
        Connection *conn = new TcpConnection(remote_addr, local_addr, newChannel, handler_);

        handler_->on_new_connection(conn, this);
    }

    void TcpAcceptor::on_write_event()
    {
        if (handler_) {
            handler_->on_quit(this);
        }

        on_close();
    }

    int TcpAcceptor::get_fd()
    {
        return channel_->get_fd();
    }

    const Socket * TcpAcceptor::get_socket() const
    {
        return socket_;
    }
}