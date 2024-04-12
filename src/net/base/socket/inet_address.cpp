#include "net/base/socket/inet_address.h"
#include "endian/endian.hpp"

#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>

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

    struct sockaddr_in InetAddress::to_ipv4_address() const
    {
        struct sockaddr_in addr;
        ::memset(&addr, 0, sizeof(struct sockaddr_in));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        addr.sin_addr.s_addr = ip_.empty() ? htonl(INADDR_ANY) : inet_addr(ip_.c_str());
        return addr;
    }

    bool operator==(const InetAddress &addr1, const InetAddress &addr2)
    {
        if (!addr1.domain_.empty() && !addr2.domain_.empty()) {
            return addr1.get_ip() == addr2.get_ip() && addr1.get_port() == addr2.get_port() && addr1.domain_ == addr2.domain_;
        }
        return addr1.get_ip() == addr2.get_ip() && addr1.get_port() == addr2.get_port();
    }

    bool operator!=(const InetAddress &addr1, const InetAddress &addr2)
    {
        return !operator==(addr1, addr2);
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
#endif
    }
}