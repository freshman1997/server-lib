#include <fcntl.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <sys/socket.h>

#include "net/socket/socket_ops.h"

namespace net::socket
{
    int create_ipv4_socket(bool noneBlock)
    {
        if (!noneBlock) {
            return ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        } else {
            return ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
        }
    }

    void close_fd(int fd)
    {
        ::close(fd);
    }

    int bind(int fd, const InetAddress &addr)
    {
        struct sockaddr_in saddr =  addr.to_ipv4_address();
        return ::bind(fd, (const struct sockaddr *)&saddr, sizeof(struct sockaddr_in));
    }

    int listen(int fd,int backlog)
    {
        return ::listen(fd, backlog);
    }

    int accept(int fd, struct sockaddr_in &peer_addr)
    {
        socklen_t ssz = (socklen_t)sizeof(peer_addr);
        return ::accept(fd, (struct sockaddr *)&peer_addr, &ssz);
    }

    int connect(int fd, const InetAddress &addr)
    {
        struct sockaddr_in saddr =  addr.to_ipv4_address();
        return ::connect(fd, (const struct sockaddr *)&saddr, sizeof(struct sockaddr_in));
    }

    void set_reuse(int fd, bool on)
    {
        int optval = on ? 1 : 0;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
               &optval, static_cast<socklen_t>(sizeof optval));
    }

    void set_no_delay(int fd, bool on)
    {
        int optval = on ? 1 : 0;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
               &optval, static_cast<socklen_t>(sizeof optval));
    }

    void set_keep_alive(int fd, bool on)
    {
        int optval = on ? 1 : 0;
        ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE,
               &optval, static_cast<socklen_t>(sizeof optval));
    }

    void set_none_block(int fd, bool on)
    {
        if (on) {
            int flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
    }
}