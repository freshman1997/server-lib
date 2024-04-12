#ifndef __NET_UDP_CONNECTION_H__
#define __NET_UDP_CONNECTION_H__

#include "buffer/linked_buffer.h"
#include "net/base/acceptor/acceptor.h"
#include "tcp_connection.h"
#include "../channel/channel.h"
#include "../handler/select_handler.h"
#include "../socket/inet_address.h"
#include "buffer/buffer.h"

namespace net
{
    const int UDP_DATA_LIMIT = 1472;

    class UdpConnection : public TcpConnection, virtual public Acceptor
    {
    public:
        UdpConnection(const std::string ip, int port, int fd);

        UdpConnection(Socket *scok);

    public:
        virtual void send();

        virtual void on_read_event();

    public:
        virtual bool listen();

        virtual void on_close();

        virtual Channel * get_channel();
    };
}

#endif