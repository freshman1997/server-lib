#ifndef __NET_TCP_CONNECTOR_H__
#define __NET_TCP_CONNECTOR_H__
#include "connector.h"
#include "net/handler/connection_handler.h"
#include <memory>

namespace yuan::net
{
    class TcpConnector : public Connector
    {
    public:
        class ConnectAttemptHandler;

        TcpConnector();
        ~TcpConnector();

    public:
        virtual bool connect(const InetAddress &address, int timeout = 10 * 1000, int retryCount = 3);

        virtual void set_data(timer::TimerManager *timerManager,
                              std::shared_ptr<ConnectorHandler> connectorHandler,
                              EventHandler *eventHander);

        virtual void set_ssl_module(std::shared_ptr<SSLModule> module);

        virtual int get_retry_count() const override;

        virtual void cancel();

    public:
        void on_connect_timeout(timer::Timer *timer);

    private:
        class TcpConnectorData;
        std::shared_ptr<TcpConnectorData> data_;
    };
}

#endif
