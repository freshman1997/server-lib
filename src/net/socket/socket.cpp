#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/tcp.h>
#include <unistd.h>

#include "net/socket/inet_address.h"
#include "net/socket/socket.h"
#include "net/socket/socket_ops.h"

namespace net
{
    Socket::Socket(const char *ip, int port) : addr(new InetAddress(ip, port)), fd_(socket::create_ipv4_socket(false))
    {}

    Socket::~Socket()
    {
        ::close(fd_);
        delete addr;
    }

    bool Socket::bind()
    {
        int res = socket::bind(fd_, *addr);
        return res == 0;
    }

    bool Socket::listen()
    {
        int res = socket::listen(fd_, 128);
        return res == 0;
    }

    int Socket::accept(struct sockaddr_in &peer_addr)
    {
        int conn_fd = socket::accept(fd_, peer_addr);
        if (conn_fd < 0) {
            // log
        }

        return conn_fd;
    }

    bool Socket::connect()
    {
        return socket::connect(fd_, *addr) == 0;
    }

    void Socket::set_no_deylay(bool on)
    {
        socket::set_no_delay(fd_, on);
    }

    void Socket::set_reuse(bool on)
    {
        socket::set_reuse(fd_, on);
    }

    void Socket::set_keep_alive(bool on)
    {
        socket::set_keep_alive(fd_, on);
    }

    void Socket::set_none_block(bool on)
    {
        socket::set_none_block(fd_, on);
    }
}