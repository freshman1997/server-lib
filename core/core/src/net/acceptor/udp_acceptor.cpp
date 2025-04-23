#include "net/acceptor/udp/udp_instance.h"
#include "net/channel/channel.h"
#include "net/connection/connection.h"
#include "net/connection/udp_connection.h"
#include "net/socket/inet_address.h"
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

#include "net/acceptor/udp_acceptor.h"
#include "net/handler/event_handler.h"
#include "net/socket/socket.h"
#include "net/handler/connection_handler.h"

namespace yuan::net 
{
    UdpAcceptor::UdpAcceptor(Socket *socket, timer::TimerManager *timerManager) : sock_(socket), timer_manager_(timerManager)
    {
        instance_.set_acceptor(this);
        channel_ = nullptr;
    }

    UdpAcceptor::~UdpAcceptor()
    {
        if (channel_) {
            channel_->disable_all();
            if (handler_) {
                handler_->close_channel(channel_);
            }
            channel_->set_handler(nullptr);
            channel_ = nullptr;
        }

        delete sock_;
        std::cout << "udp acceptor close\n";
    }

    bool UdpAcceptor::listen()
    {
        if (!sock_) {
            return false;
        }

        channel_ = new Channel;
        channel_->enable_read();
        channel_->enable_write();
        channel_->set_handler(this);
        channel_->set_fd(sock_->get_fd());

        return true;
    }

    void UdpAcceptor::close()
    {
        delete this;
    }

    Channel * UdpAcceptor::get_channel()
    {
        return channel_;
    }

    void UdpAcceptor::update_channel()
    {
        assert(channel_);
        if (handler_) {
            handler_->update_channel(channel_);
        }
    }

    void UdpAcceptor::on_read_event()
    {
        assert(sock_ && handler_);

        sockaddr_storage peer_addr;
    #ifdef _WIN32
        int size = sizeof(peer_addr);
    #else
        socklen_t size = sizeof(peer_addr);
    #endif
        ::memset(&peer_addr, 0, sizeof(peer_addr));
        auto buff = instance_.get_input_buff_list()->get_current_buffer();
        buff->reset();
        if (buff->writable_size() < UDP_DATA_LIMIT) {
            buff->resize(UDP_DATA_LIMIT);
        }
        
        bool read = false;
        int bytes = 0;
        const struct sockaddr_in *address = (struct sockaddr_in*)(&peer_addr);
        do {
            if (read) {
                buff = instance_.get_input_buff_list()->allocate_buffer();
            }
        #ifdef __linux__
            bytes = ::recvfrom(sock_->get_fd(), buff->buffer_begin(), buff->writable_size(), MSG_DONTWAIT, (struct sockaddr *)&peer_addr, &size);
        #elif defined _WIN32
            bytes = ::recvfrom(sock_->get_fd(), buff->buffer_begin(), buff->writable_size(), 0, (struct sockaddr *)&peer_addr, &size);
        #elif defined __APPLE__
            bytes = ::recvfrom(sock_->get_fd(), buff->buffer_begin(), buff->writable_size(), MSG_DONTWAIT, (struct sockaddr *)&peer_addr, &size);
        #endif
            if (bytes <= 0) {
                if (bytes == 0) {
                    break;
                }

                if (errno != EINTR && errno != EWOULDBLOCK && errno != EAGAIN) {
                    break;
                }
                
                channel_->enable_read();
                channel_->enable_write();
                handler_->update_channel(channel_);
                return;
            } else {
                buff->fill(bytes);
                read = true;
            }
        } while (bytes >= buff->writable_size());

        if (read) {
            InetAddress addr = {::inet_ntoa(address->sin_addr), ntohs(address->sin_port)};
            auto res = instance_.on_recv(addr);
            if (res.first && res.second) {
                UdpConnection *udpConn = static_cast<UdpConnection *>(res.second);
                udpConn->set_connection_handler(conn_handler_);
                udpConn->set_event_handler(handler_);
                handler_->on_new_connection(udpConn);
                udpConn->set_instance_handler(&instance_);
                udpConn->set_connection_state(ConnectionState::connected);
                res.second->on_read_event();
            } else {
                if (!res.first && res.second) {
                    res.second->abort();
                    return;
                }

                if (res.first && res.second) {
                    res.second->on_read_event();
                }
            }
        }
    } 

    void UdpAcceptor::on_write_event()
    {
        instance_.send();
    }

    int UdpAcceptor::send_to(Connection *conn, buffer::Buffer *buff)
    {
        if (buff->readable_bytes() == 0) {
            return 0;
        }

        int ret = send_to(conn->get_remote_address(), buff);
        if (ret > 0) {
            if (ret >= buff->readable_bytes()) {
                buff->reset();
            } else {
                buff->add_read_index(ret);
                std::cout << "still remains data: " << buff->readable_bytes() << " bytes.\n";
            }
        }
        return ret;
    }

    int UdpAcceptor::send_to(const InetAddress &address, buffer::Buffer *buff)
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
        assert(channel_);
        handler_->update_channel(channel_);
    }

    void UdpAcceptor::set_connection_handler(ConnectionHandler *connHandler)
    {
        conn_handler_ = connHandler;
    }
}