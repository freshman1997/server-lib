#include "net/socket/inet_address.h"
#include "endian/endian.hpp"

#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

namespace net 
{
    InetAddress::InetAddress(std::string ip, int port) : ip_(std::move(ip)), port_(port) {}

    InetAddress::InetAddress(const struct sockaddr_in &addr)
    {
        if (addr.sin_family == AF_INET) {
            port_ = endian::networkToHost16(addr.sin_port);
            ip_ = ::inet_ntoa(addr.sin_addr);
        }
    }

    struct sockaddr_in InetAddress::to_ipv4_address() const
    {
        struct sockaddr_in addr;
        ::memset(&addr, 0, sizeof(struct sockaddr_in));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        addr.sin_addr.s_addr = inet_addr("192.168.88.50");
        return addr;
    }
}