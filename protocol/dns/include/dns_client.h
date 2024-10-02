#ifndef __NET_DNS_DNS_CLIENT_H__
#define __NET_DNS_DNS_CLIENT_H__
#include "net/connection/connection.h"

namespace net::dns 
{
    class DnsClient
    {
    public:
        DnsClient();
        ~DnsClient();
        
    public:
        virtual void on_connected(Connection *conn);

        virtual void on_error(Connection *conn);

        virtual void on_read(Connection *conn);

        virtual void on_write(Connection *conn);

        virtual void on_close(Connection *conn);
    };
}

#endif