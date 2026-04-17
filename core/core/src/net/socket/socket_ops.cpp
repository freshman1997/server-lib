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
#include <cstring>
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
        if (fd >= 0 && noneBlock) {
            set_none_block(fd, true);
        }
        return fd;
    }

    int create_ipv4_udp_socket(bool noneBlock)
    {
        int fd = create_ipv4_socket(SOCK_DGRAM, IPPROTO_UDP);
        if (fd >= 0 && noneBlock) {
            set_none_block(fd, true);
        }
        return fd;
    }

    int create_ipv6_tcp_socket(bool noneBlock)
    {
        int fd = ::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
        if (fd >= 0) {
            set_ipv6_only(fd, false);
            if (noneBlock) {
                set_none_block(fd, true);
            }
        }
        return fd;
    }

    int create_ipv6_udp_socket(bool noneBlock)
    {
        int fd = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
        if (fd >= 0) {
            set_ipv6_only(fd, false);
            if (noneBlock) {
                set_none_block(fd, true);
            }
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

    int bind(int fd, const InetAddress & addr)
    {
        sockaddr_storage saddr = addr.to_sockaddr();
        socklen_t len = addr.is_ipv6() ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
        return ::bind(fd, reinterpret_cast<const sockaddr *>(&saddr), len);
    }

    int listen(int fd, int backlog)
    {
        return ::listen(fd, backlog);
    }

    int accept(int fd, sockaddr_storage & peer_addr)
    {
        socklen_t ssz = static_cast<socklen_t>(sizeof(peer_addr));
        ::memset(&peer_addr, 0, sizeof(peer_addr));
        return ::accept(fd, reinterpret_cast<sockaddr *>(&peer_addr), &ssz);
    }

    int connect(int fd, const InetAddress & addr)
    {
        sockaddr_storage saddr = addr.to_sockaddr();
        socklen_t len = addr.is_ipv6() ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
        return ::connect(fd, reinterpret_cast<const sockaddr *>(&saddr), len);
    }

    int get_last_error()
    {
#ifndef _WIN32
        return errno;
#else
        return WSAGetLastError();
#endif
    }

    bool set_reuse(int fd, bool on, bool exclude)
    {
#ifndef _WIN32
        int optval = on ? 1 : 0;
        return ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                            &optval, static_cast<socklen_t>(sizeof optval));
#else
        u_long optval = on ? 1 : 0;
        if (!exclude) {
            return ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                                reinterpret_cast<char *>(&optval), sizeof(optval)) == 0;
        } else {
            return ::setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                                reinterpret_cast<char *>(&optval), sizeof(optval)) == 0;
        }

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
        (void)ioctlsocket(fd, FIONBIO, &mode);
#endif
    }

    void set_ipv6_only(int fd, bool on)
    {
        int optval = on ? 1 : 0;
        ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,
                     &optval, static_cast<socklen_t>(sizeof(optval)));
    }
}
