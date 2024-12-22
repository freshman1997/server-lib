#ifndef __NET_CONNECTOR_H__
#define __NET_CONNECTOR_H__
#include "net/handler/connection_handler.h"
#include "net/handler/event_handler.h"
#include "net/secuity/ssl_module.h"
#include "net/socket/inet_address.h"
#include "timer/timer_manager.h"
#include <memory>

namespace yuan::net 
{
    class ConnectorHandler;

    class Connector : public ConnectionHandler
    {
    public:
        virtual bool connect(const InetAddress &address, int timeout = 10 * 1000, int retryCount = 1) = 0;

        virtual void set_data(timer::TimerManager *timerManager, ConnectorHandler *connectorHandler, EventHandler *eventHander) = 0;

        virtual void set_ssl_module(std::shared_ptr<SSLModule> module) = 0;

        virtual int get_retry_count() = 0;

        virtual void cancel() = 0;
    };
}

#endif
