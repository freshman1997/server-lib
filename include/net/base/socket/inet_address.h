#ifndef __INET_ADDRESS_H__
#define __INET_ADDRESS_H__
#include <string>
#include <netinet/in.h>

namespace net
{
    class InetAddress
    {
    public:
        InetAddress();
        
        InetAddress(std::string ip, int port);

        InetAddress(const struct sockaddr_in &);

        InetAddress(const InetAddress &addr);

        friend bool operator==(const InetAddress &addr1, const InetAddress &addr2);

        friend bool operator!=(const InetAddress &addr1, const InetAddress &addr2);

        void set_addr(const std::string &ip, int port)
        {
            ip_ = ip;
            port_ = port;
        }

        int get_port() const 
        {
            return port_;
        }

        const std::string & get_ip() const 
        {
            return ip_;
        }

        struct sockaddr_in to_ipv4_address() const;

        const std::string & get_domain() const 
        {
            return domain_;
        }

        void set_domain(const std::string &domain)
        {
            domain_ = domain;
        }

        std::string to_address_key(bool only = false) const
        {
            if (only) {
                return domain_.empty() ? ip_ + ":" + std::to_string(port_) : domain_ + ":" + std::to_string(port_);
            }
            
            return domain_ + ":" + ip_ + ":" + std::to_string(port_);
        }

    public:
        static std::string get_address_by_host(const std::string &host);

    private:
        int port_;
        std::string ip_;
        std::string domain_;
    };

    bool operator==(const InetAddress &addr1, const InetAddress &addr2);

    bool operator!=(const InetAddress &addr1, const InetAddress &addr2);
}

#endif
