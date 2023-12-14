#include <arpa/inet.h>
#include <cassert>
#include <memory>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "net/acceptor/tcp_acceptor.h"
#include "net/connection/connection.h"
#include "net/connection/tcp_connection.h"
#include "net/handler/accept_handler.h"
#include "net/channel/channel.h"
#include "net/socket/inet_address.h"
#include "net/socket/socket.h"

namespace net
{
    TcpAcceptor::TcpAcceptor() : handler_(nullptr), channel_(nullptr), socket_(nullptr)
    {}

    bool TcpAcceptor::listen()
    {
        assert(socket_);
        if (!socket_->listen()) {
            on_write_event();
            return false;
        }

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
    
    void TcpAcceptor::on_read_event()
    {
        assert(socket_);
        // TODO accept
        struct sockaddr_in peer_addr;
        int conn_fd = socket_->accept(peer_addr);
        if (conn_fd < 0) {
            on_write_event();
            return;
        }

        std::shared_ptr<InetAddress> remote_addr = std::make_shared<InetAddress>(::inet_ntoa(peer_addr.sin_addr), peer_addr.sin_port);
        std::shared_ptr<InetAddress> local_addr(socket_->get_address());
        std::shared_ptr<Channel> newChannel = std::make_shared<Channel>(conn_fd);
        Connection *conn = new TcpConnection(remote_addr, local_addr, newChannel);

        handler_->on_new_connection(conn, this);
    }

    void TcpAcceptor::on_write_event()
    {
        if (handler_) {
            handler_->on_close(this);
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