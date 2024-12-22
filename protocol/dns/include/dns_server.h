#ifndef __NET_DNS_DNS_SERVER_H__
#define __NET_DNS_DNS_SERVER_H__

#include "net/handler/connection_handler.h"
namespace yuan::net::dns 
{
    class DnsServer : public ConnectionHandler
    {
    public:
        DnsServer();
        ~DnsServer();

    public:
        virtual void on_connected(Connection *conn);

        virtual void on_error(Connection *conn);

        virtual void on_read(Connection *conn);

        virtual void on_write(Connection *conn);

        virtual void on_close(Connection *conn);

    public:
        bool serve(int port);
    };
}

#endif