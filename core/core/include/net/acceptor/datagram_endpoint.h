#ifndef __NET_ACCEPTOR_DATAGRAM_ENDPOINT_H__
#define __NET_ACCEPTOR_DATAGRAM_ENDPOINT_H__

#include "buffer/byte_buffer.h"

namespace yuan::timer
{
    class TimerManager;
}

namespace yuan::net
{
    class Channel;
    class Connection;
    class InetAddress;

    class DatagramEndpoint
    {
    public:
        virtual ~DatagramEndpoint() = default;

        virtual int send_datagram(Connection *conn, const yuan::buffer::ByteBuffer &buff) = 0;
        virtual int send_datagram(const InetAddress &addr, const yuan::buffer::ByteBuffer &buff) = 0;
        virtual Channel *endpoint_channel() const = 0;
        virtual void update_endpoint_channel() = 0;
        virtual timer::TimerManager *endpoint_timer_manager() const = 0;
    };
}

#endif
