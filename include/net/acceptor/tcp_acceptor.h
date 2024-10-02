#ifndef __TCP_ACCEPTOR_H__
#define __TCP_ACCEPTOR_H__
#include "acceptor.h"
#include "../handler/select_handler.h"
#include "../channel/channel.h"

namespace net
{
    class Socket;

    class TcpAcceptor : public Acceptor
    {
    public:
        TcpAcceptor(Socket *socket);

        ~TcpAcceptor();

    public:
        virtual bool listen();

        virtual void close();

        virtual Channel * get_channel()
        {
            return channel_;
        }

    public: // select handler
        virtual void on_read_event();

        virtual void on_write_event();

        virtual void set_event_handler(EventHandler *eventHandler);

        virtual void set_connection_handler(ConnectionHandler *connHandler);

    protected:
        Channel *channel_;
        Socket *socket_;
        EventHandler *handler_;
        ConnectionHandler *conn_handler_;
    };
}

#endif
