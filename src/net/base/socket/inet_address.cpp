#include "net/base/socket/inet_address.h"
#include "endian/endian.hpp"

#include <cstring>

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
    InetAddress::InetAddress() : port_(0), net_ip_(0)
    {
    }

    InetAddress::InetAddress(std::string ip, int port, uint32_t netIp) : ip_(std::move(ip)), port_(port), net_ip_(netIp) 
    {
        if (net_ip_ == 0) {
            set_net_ip();
        }
    }

    InetAddress::InetAddress(const struct sockaddr_in &addr)
    {
        this->port_ = endian::networkToHost16(addr.sin_port);
        this->ip_ = ::inet_ntoa(addr.sin_addr);
        this->net_ip_ = addr.sin_addr.s_addr;
    }

    InetAddress::InetAddress(const InetAddress &addr)
    {
        this->ip_ = addr.get_ip();
        this->port_ = addr.get_port();
        this->net_ip_ = addr.get_net_ip();
    }

    InetAddress::InetAddress(InetAddress &&addr)
    {
        this->ip_ = std::move(addr.get_ip());
        this->port_ = std::move(addr.get_port());
        this->net_ip_ = std::move(addr.get_net_ip());
    }

    struct sockaddr_in InetAddress::to_ipv4_address() const
    {
        struct sockaddr_in addr;
        ::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        addr.sin_addr.s_addr = ip_.empty() ? htonl(INADDR_ANY) : inet_addr(ip_.c_str());
        return addr;
    }

    const InetAddress & InetAddress::operator=(const InetAddress &other)
    {
        this->ip_ = other.get_ip();
        this->port_ = other.get_port();
        this->net_ip_ = other.get_net_ip();
        return *this;
    }

    const InetAddress & InetAddress::operator=(const InetAddress &&other)
    {
        this->ip_ = std::move(other.get_ip());
        this->port_ = std::move(other.get_port());
        this->net_ip_ = std::move(other.get_net_ip());
        return *this;
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

    void InetAddress::set_net_ip()
    {
        net_ip_ = ip_.empty() ? htonl(INADDR_ANY) : inet_addr(ip_.c_str());
    }

    uint32_t InetAddress::get_net_ip() const
    {
        return net_ip_;
    }
}