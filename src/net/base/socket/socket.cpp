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


#include "net/base/socket/inet_address.h"
#include "net/base/socket/socket.h"
#include "net/base/socket/socket_ops.h"

namespace net
{
    Socket::Socket(const char *ip, int port, bool udp, int fd)
    {
        fd_ = fd;
        addr = nullptr;
        id_ = -1;
        
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
        }
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
        int res = socket::connect(fd_, *addr);
        if (res < 0) {
            if (errno != EINPROGRESS) {
                return false;
            }

            return true;
        }

        return res == 0;
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