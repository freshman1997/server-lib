#ifdef _WIN32
#include <winsock2.h>
#include <WS2tcpip.h>
#include <windows.h>
#include <fcntl.h>
#else
#include <netinet/tcp.h>
#include <unistd.h>
#include <sys/socket.h>
#include <fcntl.h>
#endif

#include "net/socket/socket_ops.h"

namespace yuan::net::socket
{
    int create_ipv4_socket(int flag, int protocol)
    {
        return ::socket(AF_INET, flag, protocol);
    }

    int create_ipv4_tcp_socket(bool noneBlock)
    {
        int fd = create_ipv4_socket(SOCK_STREAM, IPPROTO_TCP);
        if (fd > 0 && noneBlock) {
            set_none_block(fd, true);
        }
        return fd;
    }

    int create_ipv4_udp_socket(bool noneBlock)
    {
        int fd = create_ipv4_socket(SOCK_DGRAM, IPPROTO_UDP);
        if (fd > 0 && noneBlock) {
            set_none_block(fd, true);
        }
        return fd;
    }

    void close_fd(int fd)
    {
    #ifndef _WIN32
        ::close(fd);
    #else
        ::closesocket(fd);
    #endif
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
        #ifndef _WIN32
        socklen_t ssz = (socklen_t)sizeof(peer_addr);
        return ::accept(fd, (struct sockaddr *)&peer_addr, &ssz);
        #else
        int len = sizeof(peer_addr);
        return ::accept(fd, (struct sockaddr*) &peer_addr, &len);
        #endif
    }

    int connect(int fd, const InetAddress &addr)
    {
        struct sockaddr_in saddr =  addr.to_ipv4_address();
        return ::connect(fd, (const struct sockaddr *)&saddr, sizeof(saddr));
    }

    void set_reuse(int fd, bool on)
    {
        #ifndef _WIN32
        int optval = on ? 1 : 0;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
               &optval, static_cast<socklen_t>(sizeof optval));
        #else
        u_long optval = on ? 1 : 0;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
               (char *)&optval, sizeof(optval));
        #endif
    }

    void set_no_delay(int fd, bool on)
    {
        #ifndef _WIN32
        int optval = on ? 1 : 0;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
               &optval, static_cast<socklen_t>(sizeof optval));
        #else
        u_long optval = on ? 1 : 0;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
               (char *)&optval, sizeof(optval));
        #endif
    }

    void set_keep_alive(int fd, bool on)
    {
        #ifndef _WIN32
        int optval = on ? 1 : 0;
        ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE,
               &optval, static_cast<socklen_t>(sizeof optval));
        #else
        u_long optval = on ? 1 : 0;
        ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE,
               (char *)&optval, sizeof(optval));
        #endif
    }

    void set_none_block(int fd, bool on)
    {
    #ifndef _WIN32
        if (on) {
            int flags = fcntl(fd, F_GETFL);
            fcntl(fd, F_SETFL, flags |= O_NONBLOCK);
        }
    #else
        u_long mode = on ? 1 : 0;
        if (ioctlsocket(fd, FIONBIO, &mode) == SOCKET_ERROR) {
            WSACleanup();
        }
    #endif
    }
}