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

    private:
        int port_;
        std::string ip_;
    };
}

#endif
