#ifndef __NET_UDP_CONNECTION_H__
#define __NET_UDP_CONNECTION_H__

#include "net/base/connection/connection.h"
#include "net/base/socket/inet_address.h"

namespace net
{
    class UdpConnection : public Connection
    {
    public:
        UdpConnection(const InetAddress &addr);
        ~UdpConnection();

    public:
        virtual bool is_connected();

        virtual const InetAddress & get_remote_address();

        virtual Buffer * get_input_buff(bool take = false);

        virtual Buffer * get_output_buff(bool take = false);

        virtual void send(Buffer *buff);

        virtual void send();

        // 丢弃所有未发送的数据
        virtual void abort();

        // 发送完数据后返回
        virtual void close();

        virtual ConnectionType get_conn_type();

        virtual Channel * get_channel();
        
        virtual void set_connection_handler(ConnectionHandler *handler);

        virtual const Socket * get_scoket();

    };
}

#endif