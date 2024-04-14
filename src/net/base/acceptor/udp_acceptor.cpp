#include <iostream>
#include <unistd.h>

#include "net/base/acceptor/udp_acceptor.h"
#include "net/base/handler/event_handler.h"
#include "net/base/socket/socket.h"

namespace net 
{
    UdpAcceptor::UdpAcceptor(Socket *socket) : sock_(socket)
    {
        
    }

    UdpAcceptor::~UdpAcceptor()
    {
        ::close(sock_->get_fd());
        delete sock_;
        std::cout << "udp acceptor close\n";
    }

    bool UdpAcceptor::listen()
    {
        if (!sock_) {
            return false;
        }

        channel_.enable_read();
        channel_.enable_write();
        channel_.set_fd(sock_->get_fd());

        return sock_->bind();
    }

    void UdpAcceptor::on_close()
    {
        delete this;
    }

    Channel * UdpAcceptor::get_channel()
    {
        return &channel_;
    }

    void UdpAcceptor::on_read_event()
    {

    }

    void UdpAcceptor::on_write_event()
    {
        
    }

    void UdpAcceptor::set_event_handler(EventHandler *handler)
    {
        handler_ = handler;
        handler_->update_event(&channel_);
    }
}