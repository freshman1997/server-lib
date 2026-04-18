#ifndef __TCP_ACCEPTOR_H__
#define __TCP_ACCEPTOR_H__
#include "stream_acceptor.h"
#include "../handler/select_handler.h"
#include "../channel/channel.h"
#include "net/secuity/ssl_module.h"
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
            return channel_.get();
        }

        virtual void update_channel();

    public: // select handler
        virtual void on_read_event();

        virtual void on_write_event();

        virtual void set_event_handler(EventHandler *eventHandler);
        virtual void set_connection_handler(ConnectionHandler *connHandler);
        virtual ConnectionHandler *connection_handler() const override
        {
            return conn_handler_;
        }

        virtual void set_ssl_module(std::shared_ptr<SSLModule> module);

    protected:
        std::unique_ptr<Channel> channel_;
        std::unique_ptr<Socket> socket_;
        EventHandler *handler_;
        ConnectionHandler *conn_handler_;
        std::shared_ptr<SSLModule> ssl_module_;
    };
}

#endif
