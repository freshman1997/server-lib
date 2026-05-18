#ifndef __SOCKET_OPS_H__
#define __SOCKET_OPS_H__
#include "inet_address.h"
#include "listen_options.h"

namespace yuan::net::socket
{
    int create_ipv4_socket(int flag, int protocol);

    int create_ipv4_tcp_socket(bool noneBlock = false);

    int create_ipv4_overlapped_tcp_socket(bool noneBlock = false);

    int create_ipv4_udp_socket(bool noneBlock = false);

    int create_ipv6_tcp_socket(bool noneBlock = false);

    int create_ipv6_overlapped_tcp_socket(bool noneBlock = false);

    int create_ipv6_udp_socket(bool noneBlock = false);

    void close_fd(int fd);

    int bind(int fd, const InetAddress & addr);

    int listen(int fd, int backlog);

    int accept(int fd, sockaddr_storage & peer_addr);

    int connect(int fd, const InetAddress & addr);

    InetAddress get_local_address(int fd);

    int get_last_error();

    bool set_reuse(int fd, bool on, bool exclude = false);

    bool set_reuse_addr(int fd, bool on, bool exclusive = false);

    bool set_reuse_port(int fd, bool on);

    bool apply_listen_options(int fd, const ListenOptions &options);

    void set_no_delay(int fd, bool on);

    void set_keep_alive(int fd, bool on);

    void set_none_block(int fd, bool on);

    void set_ipv6_only(int fd, bool on);

    bool shutdown_write(int fd);
}

#endif
