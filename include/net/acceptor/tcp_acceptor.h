#ifndef __TCP_ACCEPTOR_H__
#define __TCP_ACCEPTOR_H__
#include <memory>

#include "net/acceptor/acceptor.h"
#include "net/handler/select_handler.h"

namespace net
{
    class Socket;
    class Channel;

    class TcpAcceptor : public Acceptor, public SelectHandler
    {
    public:
        TcpAcceptor(Socket *socket);

    public:
        virtual bool listen();

        virtual void on_close();

        virtual void set_handler(AcceptHandler *handler);

        virtual void on_new_connection(Connection *conn);

        virtual Channel * get_channel()
        {
            return channel_;
        }

    public: // select handler
        virtual void on_read_event();

        virtual void on_write_event();

        virtual int get_fd();

    public:
        const Socket * get_socket() const;

    private:
        Channel *channel_;
        Socket *socket_;
        AcceptHandler *handler_;
    };
}

#endif
