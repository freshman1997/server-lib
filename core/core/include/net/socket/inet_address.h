#ifndef __INET_ADDRESS_H__
#define __INET_ADDRESS_H__
#include <cstdint>
#include <string>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <netinet/in.h>
#endif

namespace yuan::net
{
    enum class AddressFamily {
        ipv4,
        ipv6
    };

    class InetAddress
    {
    public:
        InetAddress();

        InetAddress(std::string ip, int port);

        InetAddress(const struct sockaddr_in &addr);

        InetAddress(const struct sockaddr_in6 &addr);

        InetAddress(const struct sockaddr_storage &addr);

        InetAddress(const InetAddress &addr);

        InetAddress(InetAddress &&addr) noexcept;

        ~InetAddress() = default;

    public:
        InetAddress &operator=(const InetAddress &other);

        InetAddress &operator=(InetAddress &&other) noexcept;

        bool operator==(const InetAddress &other) const;

        bool operator!=(const InetAddress &other) const;

        bool operator<(const InetAddress &other) const;

        void set_addr(const std::string &ip, int port)
        {
            ip_ = ip;
            port_ = port;
            init_address();
        }

        int get_port() const
        {
            return port_;
        }

        const std::string &get_ip() const
        {
            return ip_;
        }

        AddressFamily family() const
        {
            return family_;
        }

        bool is_ipv6() const
        {
            return family_ == AddressFamily::ipv6;
        }

        void init_address();

        uint32_t get_net_ip() const;

        struct sockaddr_in to_ipv4_address() const;

        struct sockaddr_in6 to_ipv6_address() const;

        struct sockaddr_storage to_sockaddr() const;

        std::string to_address_key() const
        {
            if (is_ipv6()) {
                return "[" + ip_ + "]:" + std::to_string(port_);
            }
            return ip_ + ":" + std::to_string(port_);
        }

    public:
        static std::string normalize_host(const std::string &host);

        static std::string get_address_by_host(const std::string &host);

        static std::string get_ipv4_address_by_host(const std::string &host);

        static std::string get_ipv6_address_by_host(const std::string &host);

    private:
        int port_;
        struct sockaddr_storage addr_;
        AddressFamily family_;
        std::string ip_;
    };
}

namespace std
{
#ifndef __APPLE__
    template <>
    struct hash<yuan::net::InetAddress>
    {
        size_t operator()(const yuan::net::InetAddress &address) const noexcept
        {
            std::size_t h1 = std::hash<std::string>
            {
            }
            (address.to_address_key());
            return h1;
        }
    };
#else
    template <>
    struct hash<yuan::net::InetAddress>
    {
        size_t operator()(const yuan::net::InetAddress &address) const noexcept
        {
            std::size_t h1 = std::hash<std::string>
            {
            }
            (address.to_address_key());
            return h1;
        }
    };
#endif
}

#endif
