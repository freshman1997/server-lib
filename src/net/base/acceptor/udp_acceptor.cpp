#include "net/base/connection/connection.h"
#include "net/base/connection/udp_connection.h"
#include "net/base/socket/inet_address.h"
#include <cstring>
#include <iostream>
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

#include "net/base/acceptor/udp_acceptor.h"
#include "net/base/handler/event_handler.h"
#include "net/base/socket/socket.h"
#include "net/base/handler/connection_handler.h"

namespace net 
{
    UdpAcceptor::UdpAcceptor(Socket *socket, timer::TimerManager *timerManager) : sock_(socket), timer_manager_(timerManager)
    {
        instance_.set_acceptor(this);
    }

    UdpAcceptor::~UdpAcceptor()
    {
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
        channel_.set_handler(this);
        channel_.set_fd(sock_->get_fd());

        return true;
    }

    void UdpAcceptor::close()
    {
        delete this;
    }

    Channel * UdpAcceptor::get_channel()
    {
        return &channel_;
    }

    void UdpAcceptor::on_read_event()
    {
        assert(sock_);

        sockaddr_storage peer_addr;
    #ifdef _WIN32
        int size = sizeof(peer_addr);
    #else
        socklen_t size = sizeof(peer_addr);
    #endif
        ::memset(&peer_addr, 0, sizeof(peer_addr));
        auto buff = instance_.get_input_buff_list()->get_current_buffer();
    #ifdef __linux__
        int bytes = ::recvfrom(sock_->get_fd(), buff->buffer_begin(), buff->writable_size(), MSG_DONTWAIT, (struct sockaddr *)&peer_addr, &size);
    #elif defined _WIN32
        int bytes = ::recvfrom(sock_->get_fd(), buff->buffer_begin(), buff->writable_size(), 0, (struct sockaddr *)&peer_addr, &size);
    #elif defined __APPLE__
    #endif
        const struct sockaddr_in *address = (struct sockaddr_in*)(&peer_addr);
        InetAddress addr = {::inet_ntoa(address->sin_addr), ntohs(address->sin_port)};
        if (bytes <= 0) {
            if (bytes == 0) {
                std::cout << "please check, ip: " << addr.get_ip() << ", no data read!!!";
                buff->reset();
            } else if (bytes == -1) {
                std::cout << "please check, ip: " << addr.get_ip() << ", error occured ==> " << errno << '\n';
            }
            return;
        } else {
            buff->fill(bytes);
            instance_.get_input_buff_list()->append_buffer(buff);
        }

        auto res = instance_.on_recv(addr);
        if (res.first && res.second) {
            UdpConnection *udpConn = static_cast<UdpConnection *>(res.second);
            udpConn->set_event_handler(handler_);
            handler_->on_new_connection(udpConn);
            udpConn->set_instance_handler(&instance_);
            udpConn->set_connection_state(ConnectionState::connected);
        }

        if (res.second) {
            res.second->on_read_event();
        }
    } 

    void UdpAcceptor::on_write_event()
    {
        instance_.send();
    }

    int UdpAcceptor::send_to(Connection *conn, Buffer *buff)
    {
        if (buff->readable_bytes() == 0) {
            return 0;
        }

        if (conn->get_connection_handler()) {
            conn->get_connection_handler()->on_write(conn);
        }

        int ret = send_to(conn->get_remote_address(), buff);
        if (ret > 0) {
            if (ret >= buff->readable_bytes()) {
                buff->reset();
            } else {
                buff->add_read_index(ret);
                std::cout << "still remains data: " << buff->readable_bytes() << " bytes.\n";
            }
        } else if (ret < 0) {
            instance_.on_connection_close(conn);
            if (conn->get_connection_handler()) {
                conn->get_connection_handler()->on_error(conn);
            }
            conn->abort();
        }

        return ret;
    }

    int UdpAcceptor::send_to(const InetAddress &address, Buffer *buff)
    {
        sockaddr_in addr;
        memset(&addr, 0, sizeof(sockaddr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = address.get_net_ip();
        addr.sin_port = htons(address.get_port());
        return ::sendto(sock_->get_fd(), buff->peek(), buff->readable_bytes() > UDP_DATA_LIMIT ? UDP_DATA_LIMIT : buff->readable_bytes(), 
            0, (struct sockaddr *)&addr, sizeof(addr));
    }

    void UdpAcceptor::set_event_handler(EventHandler *handler)
    {
        handler_ = handler;
        handler_->update_event(&channel_);
    }

    void UdpAcceptor::set_connection_handler(ConnectionHandler *connHandler)
    {
        conn_handler_ = connHandler;
    }
}