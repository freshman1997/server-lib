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
    class InetAddress
    {
    public:
        InetAddress();
        
        InetAddress(std::string ip, int port, uint32_t netIp = 0);

        InetAddress(const struct sockaddr_in &);

        InetAddress(const InetAddress &addr);

        InetAddress(InetAddress &&addr);

        ~InetAddress() = default;

    public:
        const InetAddress & operator=(const InetAddress &other);

        const InetAddress & operator=(const InetAddress &&other);

        bool operator==(const InetAddress &other) const;

        bool operator!=(const InetAddress &other);

        bool operator<(const InetAddress &other);

        void set_addr(const std::string &ip, int port)
        {
            ip_ = ip;
            port_ = port;
            set_net_ip();
        }

        int get_port() const 
        {
            return port_;
        }

        const std::string & get_ip() const 
        {
            return ip_;
        }

        void set_net_ip();

        uint32_t get_net_ip() const;

        struct sockaddr_in to_ipv4_address() const;

        std::string to_address_key() const
        {
            return ip_ + ":" + std::to_string(port_);
        }

    public:
        static std::string get_address_by_host(const std::string &host);

    private:
        int port_;
        uint32_t net_ip_;
        std::string ip_;
    };
}

namespace std 
{
#ifndef __APPLE__
    template<>
    struct hash<yuan::net::InetAddress> 
    {
        size_t operator()(const yuan::net::InetAddress &address) const noexcept
        {
            std::size_t h1 = std::hash<std::string>{}(address.get_ip());
            std::size_t h2 = std::hash<int>{}(address.get_port());
            return h1 ^ (h2 << 1); // or use boost::hash_combine
        }
    };
#else
    template<>
    struct hash<net::InetAddress>
    {
        size_t operator()(const yuan::net::InetAddress &address) const noexcept
        {
            std::size_t h1 = std::hash<std::string>{}(address.get_ip());
            std::size_t h2 = std::hash<int>{}(address.get_port());
            return h1 ^ (h2 << 1); // or use boost::hash_combine
        }
    };
#endif
}

#endif
