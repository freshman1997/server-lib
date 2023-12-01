#include <cstring>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/tcp.h>
#include <unistd.h>

#include "net/socket/socket.h"

namespace net
{
    Socket::~Socket()
    {
        ::close(fd_);
    }

    bool Socket::bind(const InetAddress &addr)
    {
        struct sockaddr_in saddr =  addr.to_ipv4_address();
        return ::bind(fd_, (const struct sockaddr *)&saddr, sizeof(struct sockaddr_in)) == 0;
    }

    bool Socket::listen()
    {
        return ::listen(fd_, 4096) == 0;
    }

    int Socket::accept(const InetAddress &addr)
    {
        struct sockaddr_in saddr =  addr.to_ipv4_address();
        struct sockaddr peer_addr;
        ::memset(&peer_addr, 0, sizeof(struct sockaddr));
        socklen_t ssz = (socklen_t)sizeof(struct sockaddr_in);
        int conn_fd = ::accept(fd_, (struct sockaddr *)&saddr, &ssz);
        if (conn_fd < 0) {
            // log
        }

        return conn_fd;
    }

    bool Socket::connect(const InetAddress &addr)
    {
        struct sockaddr_in saddr =  addr.to_ipv4_address();
        return :: connect(fd_, (const struct sockaddr *)&saddr, sizeof(struct sockaddr_in));
    }

    void Socket::set_no_deylay(bool on)
    {
        int optval = on ? 1 : 0;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY,
               &optval, static_cast<socklen_t>(sizeof optval));
    }

    void Socket::set_reuse(bool on)
    {
        int optval = on ? 1 : 0;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR,
               &optval, static_cast<socklen_t>(sizeof optval));
    }

    void Socket::set_keep_alive(bool on)
    {
        int optval = on ? 1 : 0;
        ::setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE,
               &optval, static_cast<socklen_t>(sizeof optval));
    }
}