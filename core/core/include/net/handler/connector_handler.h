#ifndef __NET_CONNECTOR_HANDLER_H__
#define __NET_CONNECTOR_HANDLER_H__

namespace yuan::net 
{
    class Connection;

    class ConnectorHandler
    {
    public:
        virtual void on_connect_failed(Connection *conn) = 0;

        virtual void on_connect_timeout(Connection *conn) = 0;

        virtual void on_connected_success(Connection *conn) = 0;
    };
}

#endif