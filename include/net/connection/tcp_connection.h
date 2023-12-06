#ifndef __TCP_CONNECTION_H__
#define __TCP_CONNECTION_H__
#include <memory>

#include "connection.h"

namespace net 
{
    class InetAddress;
}

namespace net::conn
{
    class TcpConnection : public Connection
    {
    public:
        TcpConnection(net::InetAddress *addr);

        std::shared_ptr<net::InetAddress> get_address() const
        {
            return addr_;
        }

    private:
        std::shared_ptr<net::InetAddress> addr_;
    };
}

#endif