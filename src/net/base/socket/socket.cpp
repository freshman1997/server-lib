#include <cctype>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

#include "net/base/socket/inet_address.h"
#include "net/base/socket/socket.h"
#include "net/base/socket/socket_ops.h"

namespace net
{
    static bool is_host(const std::string &domain)
    {
        for (const char ch : domain) {
            if (!std::isdigit(ch) && ch != '.') {
                return true;
            }
        }
        return false;
    }

    Socket::Socket(const char *ip, int port, int fd)
    {
        fd_ = fd;
        addr = nullptr;
        id_ = -1;
        
        if (is_host(ip)) {
            struct hostent *h;
            h = ::gethostbyname(ip);
            if(h) {
                struct sockaddr_in addr_in;
                memcpy(&addr_in.sin_addr.s_addr, h->h_addr,4);
                addr_in.sin_port = htons(port);
                addr = new InetAddress(addr_in);
                addr->set_domain(ip);
                if (fd < 0) {
                    fd_ = socket::create_ipv4_socket(true);
                }
            }
        } else {
            addr = new InetAddress(ip, port);
            if (fd < 0) {
                fd_ = socket::create_ipv4_socket(true);
            }
        }
    }

    Socket::~Socket()
    {
        if (fd_ > 0) {
            ::close(fd_);
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