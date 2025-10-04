#include "net/secuity/ssl_handler.h"
#ifdef _WIN32
#include <io.h>
#include <winsock2.h>
#include <windows.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <sys/socket.h>
#endif

#include "net/socket/inet_address.h"
#include "net/socket/socket.h"
#include "net/socket/socket_ops.h"

namespace yuan::net
{
    Socket::Socket(const char *ip, int port, bool udp, int fd)
    {
        fd_ = fd;
        addr = nullptr;
        
        const std::string &realIp = !*ip ? "" : InetAddress::get_address_by_host(ip);
        addr = new InetAddress(realIp.c_str(), port);
        if (fd < 0) {
            if (!udp) {
                fd_ = socket::create_ipv4_tcp_socket(true);
            } else {
                fd_ = socket::create_ipv4_udp_socket(true);
            }
        }
    }

    Socket::~Socket()
    {
        if (fd_ > 0) {
            socket::close_fd(fd_);
        }

        if (addr) {
            delete addr;
            addr = nullptr;
        }
    }

    bool Socket::bind() const
    {
        return socket::bind(fd_, *addr) == 0;
    }

    bool Socket::listen() const
    {
        return socket::listen(fd_, 128) == 0;
    }

    int Socket::accept(struct sockaddr_in &peer_addr) const
    {
        const int conn_fd = socket::accept(fd_, peer_addr);
        if (conn_fd < 0) {
            // log
        }

        return conn_fd;
    }

    bool Socket::connect(const std::shared_ptr<SSLHandler> &sslModule) const
    {
        int res = -1;
        if (sslModule) {
            res = sslModule->ssl_init_action();
        } else {
            res = socket::connect(fd_, *addr);
        }
        if (res < 0) {
        #ifdef _WIN32
            if (WSAEWOULDBLOCK == WSAGetLastError()) {
                return true;
            }
        #endif
            if (errno != EINPROGRESS) {
                return false;
            }
            return true;
        }

        return res == 0;
    }

    void Socket::set_no_delay(const bool on) const
    {
        socket::set_no_delay(fd_, on);
    }

    bool Socket::set_reuse(const bool on, const bool exclude) const
    {
        return socket::set_reuse(fd_, on, exclude);
    }

    void Socket::set_keep_alive(const bool on) const
    {
        socket::set_keep_alive(fd_, on);
    }

    void Socket::set_none_block(const bool on) const
    {
        socket::set_none_block(fd_, on);
    }
}