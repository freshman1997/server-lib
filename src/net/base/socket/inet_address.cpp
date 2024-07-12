#include "net/base/socket/inet_address.h"
#include "endian/endian.hpp"

#include <cstring>
#include <functional>

#ifdef _WIN32
#include <inaddr.h>
#include <winsock.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif

namespace net 
{
    InetAddress::InetAddress() : port_(0)
    {
    }

    InetAddress::InetAddress(std::string ip, int port) : ip_(std::move(ip)), port_(port) {}

    InetAddress::InetAddress(const struct sockaddr_in &addr)
    {
        port_ = endian::networkToHost16(addr.sin_port);
        ip_ = ::inet_ntoa(addr.sin_addr);
    }

    InetAddress::InetAddress(const InetAddress &addr)
    {
        this->ip_ = addr.get_ip();
        this->port_ = addr.get_port();
    }

    /*InetAddress::InetAddress(InetAddress &&addr)
    {
        this->ip_ = std::move(addr.get_ip());
        this->port_ = std::move(addr.get_port());
    }*/

    struct sockaddr_in InetAddress::to_ipv4_address() const
    {
        struct sockaddr_in addr;
        ::memset(&addr, 0, sizeof(struct sockaddr_in));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        addr.sin_addr.s_addr = ip_.empty() ? htonl(INADDR_ANY) : inet_addr(ip_.c_str());
        return addr;
    }

    bool InetAddress::operator==(const InetAddress &other) const
    {
        return get_ip() == other.get_ip() && get_port() == other.get_port();
    }

    bool InetAddress::operator!=(const InetAddress &other)
    {
        return !operator==(other);
    }

    bool InetAddress::operator<(const InetAddress &other)
    {
        if (ip_ != other.ip_) {
            return ip_ < other.ip_;
        }
        return port_ < other.port_;
    }

    std::string InetAddress::get_address_by_host(const std::string &host)
    {
#ifndef _WIN32
        struct hostent *h = ::gethostbyname(host.c_str());
        if(h) {
            struct sockaddr_in addr_in;
            memcpy(&addr_in.sin_addr.s_addr, h->h_addr,4);
            return ::inet_ntoa(addr_in.sin_addr);
        } else {
            return "";
        }
#else
    char *pstr;
    hostent *phost = gethostbyname(host.c_str());
    int i = 0;
    for (pstr = phost->h_addr_list[0]; pstr != nullptr; pstr = phost->h_addr_list[++i]) {
        u_long tmp = *(u_long *)pstr;
        in_addr addr;
        addr.S_un.S_addr = tmp;
        return ::inet_ntoa(addr);
    }
    return "";
#endif
    }
}