#ifndef __TCP_ACCEPTOR_H__
#define __TCP_ACCEPTOR_H__
#include "stream_acceptor.h"
#include "../handler/select_handler.h"
#include "../channel/channel.h"
#include "net/security/ssl_module.h"
#include <memory>

namespace yuan::net
{
    class Socket;

    class TcpAcceptor : public StreamAcceptor
    {
    public:
        TcpAcceptor(Socket *socket);

        ~TcpAcceptor();

    public:
        virtual bool listen();

        virtual void close();

        virtual Channel *listener_channel() const override
        {
            return channel_ ? &*channel_ : nullptr;
        }

        virtual void update_channel();

    public: // select handler
        virtual void on_read_event();

        virtual void on_write_event();

        virtual void set_event_handler(EventHandler *eventHandler);
        virtual void set_connection_handler(std::shared_ptr<ConnectionHandler> connHandler) override;
        virtual ConnectionHandler *connection_handler() const override
        {
            return conn_handler_owner_ ? &*conn_handler_owner_ : nullptr;
        }
        virtual std::shared_ptr<ConnectionHandler> connection_handler_owner() const override
        {
            return conn_handler_owner_;
        }

        virtual void set_ssl_module(std::shared_ptr<SSLModule> module);

    protected:
        std::unique_ptr<Channel> channel_;
        std::unique_ptr<Socket> socket_;
        std::shared_ptr<SelectHandler> self_handler_owner_;
        EventHandler *handler_;
        std::shared_ptr<ConnectionHandler> conn_handler_owner_;
        std::shared_ptr<SSLModule> ssl_module_;
    };
}

#endif
