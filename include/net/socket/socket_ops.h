#ifndef __SOCKET_OPS_H__
#define __SOCKET_OPS_H__
#include "inet_address.h"

namespace net::socket
{
    int create_ipv4_socket(int flag, int protocol);

    int create_ipv4_tcp_socket(bool noneBlock = false);

    int create_ipv4_udp_socket(bool noneBlock = false);


    void close_fd(int fd);

    int bind(int fd, const InetAddress &addr);

    int listen(int fd,int backlog);

    int accept(int fd, struct sockaddr_in &peer_addr);

    int connect(int fd, const InetAddress &addr);

    void set_reuse(int fd, bool on);

    void set_no_delay(int fd, bool on);

    void set_keep_alive(int fd, bool on);

    void set_none_block(int fd, bool on);
}

#endif
