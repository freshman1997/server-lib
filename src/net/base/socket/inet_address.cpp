#include "net/base/socket/inet_address.h"
#include "endian/endian.hpp"

#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

namespace net 
{
    InetAddress::InetAddress() : port_(0)
    {
    }

    InetAddress::InetAddress(std::string ip, int port) : ip_(std::move(ip)), port_(port) {}

    InetAddress::InetAddress(const struct sockaddr_in &addr)
    {
        if (addr.sin_family == AF_INET) {
            port_ = endian::networkToHost16(addr.sin_port);
            ip_ = ::inet_ntoa(addr.sin_addr);
        }
    }

    InetAddress::InetAddress(const InetAddress &addr)
    {
        this->ip_ = addr.get_ip();
        this->port_ = addr.get_port();
    }

    struct sockaddr_in InetAddress::to_ipv4_address() const
    {
        struct sockaddr_in addr;
        ::memset(&addr, 0, sizeof(struct sockaddr_in));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        return addr;
    }
}