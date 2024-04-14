#ifndef __NET_UDP_CONNECTION_H__
#define __NET_UDP_CONNECTION_H__

#include "tcp_connection.h"

namespace net
{
    const int UDP_DATA_LIMIT = 1472;

    class UdpConnection : public TcpConnection
    {
    public:
        UdpConnection(const std::string ip, int port, int fd);

        UdpConnection(Socket *scok);

    public:
        virtual void send();

        virtual void on_read_event();
    };
}

#endif