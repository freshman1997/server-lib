#ifndef __NET_TCP_CONNECTOR_H__
#define __NET_TCP_CONNECTOR_H__
#include "connector.h"
#include <memory>

namespace net 
{
    class TcpConnector : public Connector
    {
    public:
        TcpConnector();
        ~TcpConnector();

    public:
        virtual void on_connected(Connection *conn);

        virtual void on_error(Connection *conn);

        virtual void on_read(Connection *conn);

        virtual void on_write(Connection *conn);

        virtual void on_close(Connection *conn);

    public:
        virtual bool connect(const InetAddress &address, int timeout = 10 * 1000, int retryCount = 3);

        virtual void set_data(timer::TimerManager *timerManager, ConnectorHandler *connectorHandler, EventHandler *eventHander);

        virtual void set_ssl_module(std::shared_ptr<SSLModule> module);

        virtual int get_retry_count();

        virtual void cancel();

    public:
        void on_connect_timeout(timer::Timer *timer);

    private:
        class TcpConnectorData;
        std::unique_ptr<TcpConnectorData> data_;
    };
}

#endif 
