#ifndef __NET_CONNECTOR_HANDLER_H__
#define __NET_CONNECTOR_HANDLER_H__

#include <memory>

namespace yuan::net 
{
    class Connection;

    class ConnectorHandler
    {
    public:
        virtual void on_connect_failed(const std::shared_ptr<Connection> &conn) = 0;

        virtual void on_connect_timeout(const std::shared_ptr<Connection> &conn) = 0;

        virtual void on_connected_success(const std::shared_ptr<Connection> &conn) = 0;
    };
}

#endif
