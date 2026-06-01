#include "net/acceptor/udp/udp_instance.h"
#include "logger.h"
#include "net/channel/channel.h"
#include "net/connection/connection.h"
#include "net/connection/datagram_transport.h"
#include "net/socket/inet_address.h"
#include "native_platform.h"
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
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#endif

#include "net/acceptor/udp_acceptor.h"
#include "net/handler/event_handler.h"
#include "net/socket/socket.h"
#include "net/handler/connection_handler.h"

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
    }

    UdpAcceptor::UdpAcceptor(Socket * socket, timer::TimerManager * timerManager)
        : sock_(socket), timer_manager_(timerManager)
    {
        handler_ = nullptr;
        self_handler_owner_ = std::shared_ptr<SelectHandler>(this, [](SelectHandler *) {});
    }

    UdpAcceptor::~UdpAcceptor()
    {
        close();
        channel_.reset();
        self_handler_owner_.reset();
        instance_.reset();
        sock_.reset();
        LOG_INFO("udp acceptor close");
    }

    bool UdpAcceptor::listen()
    {
        if (!sock_) {
            return false;
        }

        if (!sock_->valid()) {
            return false;
        }

        instance_ = std::make_unique<UdpInstance>(this);
        //instance_->set_adapter_type(adapterType);
        channel_ = std::make_unique<Channel>();
        channel_->enable_read();
        channel_->clear_handler();
        channel_->set_fd(sock_->get_fd());

        return true;
    }

    void UdpAcceptor::close()
    {
        if (channel_) {
            channel_->disable_all();
            if (handler_) {
                handler_->close_channel(ptr_of(channel_));
                handler_ = nullptr;
            }
            channel_->clear_handler();
        }
    }

    void UdpAcceptor::update_channel()
    {
        assert(channel_);
        if (handler_) {
            handler_->update_channel(ptr_of(channel_));
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

        int bytes;
        do {
            ::memset(&peer_addr, 0, sizeof(peer_addr));
#ifdef _WIN32
            size = sizeof(peer_addr);
#else
            size = sizeof(peer_addr);
#endif

            ::yuan::buffer::ByteBuffer packet(UDP_DATA_LIMIT);

#ifdef __linux__
            bytes = ::recvfrom(sock_->get_fd(), packet.write_ptr(), static_cast<int>(packet.writable_bytes()), MSG_DONTWAIT, (struct sockaddr *)&peer_addr, &size);
#elif defined _WIN32
            bytes = ::recvfrom(sock_->get_fd(), packet.write_ptr(), static_cast<int>(packet.writable_bytes()), 0, (struct sockaddr *)&peer_addr, &size);
#elif defined __APPLE__
            bytes = ::recvfrom(sock_->get_fd(), packet.write_ptr(), static_cast<int>(packet.writable_bytes()), MSG_DONTWAIT, (struct sockaddr *)&peer_addr, &size);
#endif
            if (bytes <= 0) {
                if (bytes == 0) {
                    break;
                }

#ifdef _WIN32
                int err = app::GetLastNativeError();
                if (!app::IsNativeRetryableError(err)) {
#else
                int err = app::GetLastNativeError();
                if (!app::IsNativeRetryableError(err)) {
#endif
                    break;
                }

                channel_->enable_read();
                handler_->update_channel(ptr_of(channel_));
                break;
            }

            packet.commit(static_cast<std::size_t>(bytes));
            InetAddress addr(peer_addr);

            // Dispatch this datagram to the correct peer connection immediately
            instance_->set_input_packet(std::move(packet));

            auto res = instance_->on_recv(addr);
            if (res.first && res.second) {
                if (!res.second->is_connected()) {
                    auto *datagram = dynamic_cast<DatagramTransport *>(&*res.second);
                    if (!datagram) {
                        res.second->abort();
                        break;
                    }
                    if (conn_handler_owner_) {
                        res.second->set_connection_handler(conn_handler_owner_);
                    }
                    res.second->set_event_handler(handler_);
                    handler_->on_new_connection(res.second);
                    if (conn_handler_owner_) {
                        conn_handler_owner_->on_connected(res.second);
                    }
                    datagram->set_datagram_state(ConnectionState::connected);
                }
                res.second->on_read_event();
            } else if (!res.first && res.second) {
                res.second->abort();
            }
        } while (bytes > 0);
    }

    void UdpAcceptor::on_write_event()
    {
        instance_->send();
        if (channel_) {
            channel_->disable_write();
            update_channel();
        }
    }

    int UdpAcceptor::send_to(Connection * conn, const ::yuan::buffer::ByteBuffer & buff)
    {
        if (buff.readable_bytes() == 0) {
            return 0;
        }

        return send_to(conn->get_remote_address(), buff);
    }

    int UdpAcceptor::send_to(const InetAddress & address, const ::yuan::buffer::ByteBuffer & buff)
    {
        sockaddr_storage addr_storage = address.to_sockaddr();
        socklen_t addr_len = address.is_ipv6() ? sizeof(::sockaddr_in6) : sizeof(::sockaddr_in);
        const auto send_size = (std::min)(buff.readable_bytes(), static_cast<std::size_t>(UDP_DATA_LIMIT));
        return ::sendto(sock_->get_fd(), buff.read_ptr(), static_cast<int>(send_size),
                        0, (struct sockaddr *)&addr_storage, addr_len);
    }

    int UdpAcceptor::send_datagram(const InetAddress & address, const ::yuan::buffer::ByteBuffer & buff)
    {
        return send_to(address, buff);
    }

    void UdpAcceptor::set_event_handler(EventHandler * handler)
    {
        if (handler_ == handler) {
            if (handler_ && channel_) {
                handler_->update_channel(ptr_of(channel_));
            }
            return;
        }

        if (handler_ && handler_ != handler && channel_) {
            LOG_WARN("udp acceptor event handler switched, fd: {}", channel_->get_fd());
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

    void UdpAcceptor::set_connection_handler(std::shared_ptr<ConnectionHandler> connHandler)
    {
        conn_handler_owner_ = std::move(connHandler);
    }
}
