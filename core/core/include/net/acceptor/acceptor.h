#ifndef __ACCEPTOR_H__
#define __ACCEPTOR_H__
#include "../handler/select_handler.h"
#include "net/security/ssl_module.h"
#include <memory>

namespace yuan::net
{
    class EventHandler;
    class Connection;
    class Channel;
    class ConnectionHandler;

    class Acceptor : public SelectHandler
    {
    public:
        virtual bool listen() = 0;

        virtual void close() = 0;

        virtual void update_channel() = 0;

        virtual void set_connection_handler(std::shared_ptr<ConnectionHandler> connHandler) = 0;
        virtual ConnectionHandler *connection_handler() const = 0;
        virtual std::shared_ptr<ConnectionHandler> connection_handler_owner() const
        {
            return nullptr;
        }

        virtual void set_ssl_module(std::shared_ptr<SSLModule> module) = 0;
    };
}

#endif
