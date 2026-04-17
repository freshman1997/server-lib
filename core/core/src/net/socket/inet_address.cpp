#include "net/socket/inet_address.h"
#include "endian/endian.hpp"

#include <array>
#include <cassert>
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
    InetAddress::InetAddress()
        : port_(0), addr_{}, family_(AddressFamily::ipv4), ip_("0.0.0.0")
    {
        auto *addr4 = reinterpret_cast<sockaddr_in *>(&addr_);
        addr4->sin_family = AF_INET;
        addr4->sin_port = endian::hostToNetwork16(static_cast<uint16_t>(port_));
        addr4->sin_addr.s_addr = endian::hostToNetwork32(INADDR_ANY);
    }

    InetAddress::InetAddress(std::string ip, int port)
        : ip_(std::move(ip)), port_(port), addr_{}, family_(AddressFamily::ipv4)
    {
        init_address();
    }

    InetAddress::InetAddress(const struct sockaddr_in & addr4)
    {
        ::memset(&addr_, 0, sizeof(addr_));
        ::memcpy(&addr_, &addr4, sizeof(addr4));
        family_ = AddressFamily::ipv4;
        port_ = endian::networkToHost16(addr4.sin_port);
        std::array<char, INET6_ADDRSTRLEN> ip_buf{};
        if (::inet_ntop(AF_INET, &addr4.sin_addr, ip_buf.data(), static_cast<socklen_t>(ip_buf.size())) != nullptr) {
            ip_ = ip_buf.data();
        }
    }

    InetAddress::InetAddress(const struct sockaddr_in6 & addr6)
    {
        ::memset(&addr_, 0, sizeof(addr_));
        ::memcpy(&addr_, &addr6, sizeof(addr6));
        family_ = AddressFamily::ipv6;
        port_ = endian::networkToHost16(addr6.sin6_port);
        std::array<char, INET6_ADDRSTRLEN> ip_buf{};
        if (::inet_ntop(AF_INET6, &addr6.sin6_addr, ip_buf.data(), static_cast<socklen_t>(ip_buf.size())) != nullptr) {
            ip_ = ip_buf.data();
        }
    }

    InetAddress::InetAddress(const struct sockaddr_storage & storage)
    {
        ::memset(&addr_, 0, sizeof(addr_));
        ::memcpy(&addr_, &storage, sizeof(storage));
        if (storage.ss_family == AF_INET6) {
            family_ = AddressFamily::ipv6;
            const auto *addr6 = reinterpret_cast<const sockaddr_in6 *>(&storage);
            port_ = endian::networkToHost16(addr6->sin6_port);
            std::array<char, INET6_ADDRSTRLEN> ip_buf{};
            if (::inet_ntop(AF_INET6, &addr6->sin6_addr, ip_buf.data(), static_cast<socklen_t>(ip_buf.size())) != nullptr) {
                ip_ = ip_buf.data();
            }
        } else {
            family_ = AddressFamily::ipv4;
            const auto *addr4 = reinterpret_cast<const sockaddr_in *>(&storage);
            port_ = endian::networkToHost16(addr4->sin_port);
            std::array<char, INET6_ADDRSTRLEN> ip_buf{};
            if (::inet_ntop(AF_INET, &addr4->sin_addr, ip_buf.data(), static_cast<socklen_t>(ip_buf.size())) != nullptr) {
                ip_ = ip_buf.data();
            }
        }
    }

    InetAddress::InetAddress(const InetAddress & other)
    {
        this->ip_ = other.ip_;
        this->port_ = other.port_;
        this->family_ = other.family_;
        ::memcpy(&this->addr_, &other.addr_, sizeof(addr_));
    }

    InetAddress::InetAddress(InetAddress && other) noexcept
    {
        this->ip_ = std::move(other.ip_);
        this->port_ = other.port_;
        this->family_ = other.family_;
        ::memcpy(&this->addr_, &other.addr_, sizeof(addr_));
    }

    struct sockaddr_in InetAddress::to_ipv4_address() const
    {
        assert(family_ == AddressFamily::ipv4);
        struct sockaddr_in addr;
        ::memset(&addr, 0, sizeof(addr));
        const auto *stored = reinterpret_cast<const sockaddr_in *>(&addr_);
        addr = *stored;
        return addr;
    }

    struct sockaddr_in6 InetAddress::to_ipv6_address() const
    {
        assert(family_ == AddressFamily::ipv6);
        struct sockaddr_in6 addr;
        ::memset(&addr, 0, sizeof(addr));
        const auto *stored = reinterpret_cast<const sockaddr_in6 *>(&addr_);
        addr = *stored;
        return addr;
    }

    struct sockaddr_storage InetAddress::to_sockaddr() const
    {
        return addr_;
    }

    InetAddress &InetAddress::operator=(const InetAddress & other)
    {
        if (this != &other) {
            this->ip_ = other.ip_;
            this->port_ = other.port_;
            this->family_ = other.family_;
            ::memcpy(&this->addr_, &other.addr_, sizeof(addr_));
        }
        return *this;
    }

    InetAddress &InetAddress::operator=(InetAddress && other) noexcept
    {
        if (this != &other) {
            this->ip_ = std::move(other.ip_);
            this->port_ = other.port_;
            this->family_ = other.family_;
            ::memcpy(&this->addr_, &other.addr_, sizeof(addr_));
        }
        return *this;
    }

    bool InetAddress::operator==(const InetAddress & other) const
    {
        return family_ == other.family_ && ip_ == other.ip_ && port_ == other.port_;
    }

    bool InetAddress::operator!=(const InetAddress & other) const
    {
        return !operator==(other);
    }

    bool InetAddress::operator<(const InetAddress & other) const
    {
        if (family_ != other.family_) {
            return family_ < other.family_;
        }
        if (ip_ != other.ip_) {
            return ip_ < other.ip_;
        }
        return port_ < other.port_;
    }

    std::string InetAddress::normalize_host(const std::string & host)
    {
        if (host.empty()) {
            return "";
        }

        in6_addr addr6{};
        if (::inet_pton(AF_INET6, host.c_str(), &addr6) == 1) {
            return host;
        }

        in_addr addr4{};
        if (::inet_pton(AF_INET, host.c_str(), &addr4) == 1) {
            return host;
        }

        return get_address_by_host(host);
    }

    std::string InetAddress::get_address_by_host(const std::string & host)
    {
        if (host.empty()) {
            return "";
        }

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        addrinfo *result = nullptr;
        if (::getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || result == nullptr) {
            return "";
        }

        std::array<char, INET6_ADDRSTRLEN> ip_buf{};
        std::string resolved_ip;
        if (result->ai_family == AF_INET6) {
            const auto *sock_addr = reinterpret_cast<sockaddr_in6 *>(result->ai_addr);
            if (::inet_ntop(AF_INET6, &sock_addr->sin6_addr, ip_buf.data(), static_cast<socklen_t>(ip_buf.size())) != nullptr) {
                resolved_ip = ip_buf.data();
            }
        } else {
            const auto *sock_addr = reinterpret_cast<sockaddr_in *>(result->ai_addr);
            if (::inet_ntop(AF_INET, &sock_addr->sin_addr, ip_buf.data(), static_cast<socklen_t>(ip_buf.size())) != nullptr) {
                resolved_ip = ip_buf.data();
            }
        }

        ::freeaddrinfo(result);
        return resolved_ip;
    }

    std::string InetAddress::get_ipv4_address_by_host(const std::string & host)
    {
        if (host.empty()) {
            return "";
        }

        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        addrinfo *result = nullptr;
        if (::getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || result == nullptr) {
            return "";
        }

        std::array<char, INET6_ADDRSTRLEN> ip_buf{};
        std::string resolved_ip;
        const auto *sock_addr = reinterpret_cast<sockaddr_in *>(result->ai_addr);
        if (::inet_ntop(AF_INET, &sock_addr->sin_addr, ip_buf.data(), static_cast<socklen_t>(ip_buf.size())) != nullptr) {
            resolved_ip = ip_buf.data();
        }

        ::freeaddrinfo(result);
        return resolved_ip;
    }

    std::string InetAddress::get_ipv6_address_by_host(const std::string & host)
    {
        if (host.empty()) {
            return "";
        }

        addrinfo hints{};
        hints.ai_family = AF_INET6;
        hints.ai_socktype = SOCK_STREAM;

        addrinfo *result = nullptr;
        if (::getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || result == nullptr) {
            return "";
        }

        std::array<char, INET6_ADDRSTRLEN> ip_buf{};
        std::string resolved_ip;
        const auto *sock_addr = reinterpret_cast<sockaddr_in6 *>(result->ai_addr);
        if (::inet_ntop(AF_INET6, &sock_addr->sin6_addr, ip_buf.data(), static_cast<socklen_t>(ip_buf.size())) != nullptr) {
            resolved_ip = ip_buf.data();
        }

        ::freeaddrinfo(result);
        return resolved_ip;
    }

    void InetAddress::init_address()
    {
        ::memset(&addr_, 0, sizeof(addr_));

        if (ip_.empty()) {
            family_ = AddressFamily::ipv4;
            auto *addr4 = reinterpret_cast<sockaddr_in *>(&addr_);
            addr4->sin_family = AF_INET;
            addr4->sin_port = endian::hostToNetwork16(static_cast<uint16_t>(port_));
            addr4->sin_addr.s_addr = endian::hostToNetwork32(INADDR_ANY);
            ip_ = "0.0.0.0";
            return;
        }

        in6_addr addr6{};
        if (::inet_pton(AF_INET6, ip_.c_str(), &addr6) == 1) {
            family_ = AddressFamily::ipv6;
            auto *sa6 = reinterpret_cast<sockaddr_in6 *>(&addr_);
            sa6->sin6_family = AF_INET6;
            sa6->sin6_port = endian::hostToNetwork16(static_cast<uint16_t>(port_));
            sa6->sin6_addr = addr6;
            return;
        }

        in_addr addr4{};
        if (::inet_pton(AF_INET, ip_.c_str(), &addr4) == 1) {
            family_ = AddressFamily::ipv4;
            auto *sa4 = reinterpret_cast<sockaddr_in *>(&addr_);
            sa4->sin_family = AF_INET;
            sa4->sin_port = endian::hostToNetwork16(static_cast<uint16_t>(port_));
            sa4->sin_addr = addr4;
            return;
        }

        const auto normalized = normalize_host(ip_);
        if (!normalized.empty()) {
            ip_ = normalized;
            if (::inet_pton(AF_INET6, ip_.c_str(), &addr6) == 1) {
                family_ = AddressFamily::ipv6;
                auto *sa6 = reinterpret_cast<sockaddr_in6 *>(&addr_);
                sa6->sin6_family = AF_INET6;
                sa6->sin6_port = endian::hostToNetwork16(static_cast<uint16_t>(port_));
                sa6->sin6_addr = addr6;
                return;
            }
            if (::inet_pton(AF_INET, ip_.c_str(), &addr4) == 1) {
                family_ = AddressFamily::ipv4;
                auto *sa4 = reinterpret_cast<sockaddr_in *>(&addr_);
                sa4->sin_family = AF_INET;
                sa4->sin_port = endian::hostToNetwork16(static_cast<uint16_t>(port_));
                sa4->sin_addr = addr4;
                return;
            }
        }

        family_ = AddressFamily::ipv4;
        auto *sa4 = reinterpret_cast<sockaddr_in *>(&addr_);
        sa4->sin_family = AF_INET;
        sa4->sin_port = endian::hostToNetwork16(static_cast<uint16_t>(port_));
        sa4->sin_addr.s_addr = endian::hostToNetwork32(INADDR_ANY);
    }

    uint32_t InetAddress::get_net_ip() const
    {
        assert(family_ == AddressFamily::ipv4);
        const auto *addr4 = reinterpret_cast<const sockaddr_in *>(&addr_);
        return addr4->sin_addr.s_addr;
    }
}
