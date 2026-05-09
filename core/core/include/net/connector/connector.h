#ifndef __NET_CONNECTOR_H__
#define __NET_CONNECTOR_H__
#include "net/handler/event_handler.h"
#include "net/security/ssl_module.h"
#include "net/socket/inet_address.h"
#include "timer/timer_manager.h"
#include "net/handler/connector_handler.h"
#include <memory>

namespace yuan::net
{
    class ConnectorHandler;

    class Connector
    {
    public:
        virtual ~Connector() = default;

        virtual bool connect(const InetAddress &address, int timeout = 10 * 1000, int retryCount = 1) = 0;

        virtual void set_data(timer::TimerManager *timerManager,
                              std::shared_ptr<ConnectorHandler> connectorHandler,
                              EventHandler *eventHander) = 0;

        virtual void set_ssl_module(std::shared_ptr<SSLModule> module) = 0;

        virtual int get_retry_count() const = 0;

        virtual void cancel() = 0;
    };
}

#endif
