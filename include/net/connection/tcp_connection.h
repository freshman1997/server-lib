#ifndef __TCP_CONNECTION_H__
#define __TCP_CONNECTION_H__
#include <memory>

#include "connection.h"
#include "net/channel/channel.h"
#include "net/socket/inet_address.h"

namespace net
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
        std::shared_ptr<net::Channel> channel_;
    };
}

#endif