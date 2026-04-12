#include "net/socket/inet_address.h"
#include "endian/endian.hpp"

#include <array>
#include <cstring>

#ifdef _WIN32
#include <inaddr.h>
#include <winsock.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif

namespace yuan::net 
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
        this->net_ip_ = addr.sin_addr.s_addr;
        std::array<char, INET_ADDRSTRLEN> ip_buf {};
        if (::inet_ntop(AF_INET, &addr.sin_addr, ip_buf.data(), static_cast<socklen_t>(ip_buf.size())) != nullptr) {
            this->ip_ = ip_buf.data();
        }
    }

    InetAddress::InetAddress(const InetAddress &addr)
    {
        this->ip_ = addr.get_ip();
        this->port_ = addr.get_port();
        this->net_ip_ = addr.get_net_ip();
    }

    InetAddress::InetAddress(InetAddress &&addr)
    {
        this->ip_ = std::move(addr.ip_);
        this->port_ = addr.port_;
        this->net_ip_ = addr.net_ip_;
    }

    struct sockaddr_in InetAddress::to_ipv4_address() const
    {
        struct sockaddr_in addr;
        ::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        addr.sin_addr.s_addr = ip_.empty() ? htonl(INADDR_ANY) : net_ip_;
        return addr;
    }

    InetAddress & InetAddress::operator=(const InetAddress &other)
    {
        if (this != &other) {
            this->ip_ = other.ip_;
            this->port_ = other.port_;
            this->net_ip_ = other.net_ip_;
        }
        return *this;
    }

    InetAddress & InetAddress::operator=(InetAddress &&other) noexcept
    {
        if (this != &other) {
            this->ip_ = std::move(other.ip_);
            this->port_ = other.port_;
            this->net_ip_ = other.net_ip_;
        }
        return *this;
    }

    bool InetAddress::operator==(const InetAddress &other) const
    {
        return get_ip() == other.get_ip() && get_port() == other.get_port();
    }

    bool InetAddress::operator!=(const InetAddress &other) const
    {
        return !operator==(other);
    }

    bool InetAddress::operator<(const InetAddress &other) const
    {
        if (ip_ != other.ip_) {
            return ip_ < other.ip_;
        }
        return port_ < other.port_;
    }

    std::string InetAddress::normalize_host(const std::string &host)
    {
        if (host.empty()) {
            return "";
        }

        in_addr addr {};
        if (::inet_pton(AF_INET, host.c_str(), &addr) == 1) {
            return host;
        }

        return get_address_by_host(host);
    }

    std::string InetAddress::get_address_by_host(const std::string &host)
    {
        if (host.empty()) {
            return "";
        }

        addrinfo hints {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        addrinfo *result = nullptr;
        if (::getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || result == nullptr) {
            return "";
        }

        std::array<char, INET_ADDRSTRLEN> ip_buf {};
        const auto *sock_addr = reinterpret_cast<sockaddr_in *>(result->ai_addr);
        std::string resolved_ip;
        if (::inet_ntop(AF_INET, &sock_addr->sin_addr, ip_buf.data(), static_cast<socklen_t>(ip_buf.size())) != nullptr) {
            resolved_ip = ip_buf.data();
        }

        ::freeaddrinfo(result);
        return resolved_ip;
    }

    void InetAddress::set_net_ip()
    {
        if (ip_.empty()) {
            net_ip_ = htonl(INADDR_ANY);
            return;
        }

        in_addr addr {};
        if (::inet_pton(AF_INET, ip_.c_str(), &addr) == 1) {
            net_ip_ = addr.s_addr;
            return;
        }

        const auto normalized = normalize_host(ip_);
        if (!normalized.empty() && ::inet_pton(AF_INET, normalized.c_str(), &addr) == 1) {
            ip_ = normalized;
            net_ip_ = addr.s_addr;
            return;
        }

        net_ip_ = 0;
    }

    uint32_t InetAddress::get_net_ip() const
    {
        return net_ip_;
    }
}
