#ifndef __TCP_ACCEPTOR_H__
#define __TCP_ACCEPTOR_H__
#include <memory>

#include "net/acceptor/acceptor.h"
#include "net/handler/select_handler.h"
#include "net/channel/channel.h"

namespace net
{
    class Socket;
    class Channel;

    class TcpAcceptor : public Acceptor
    {
    public:
        TcpAcceptor(Socket *socket);

        ~TcpAcceptor();

    public:
        virtual bool listen();

        virtual void on_close();

        virtual Channel * get_channel()
        {
            return &channel_;
        }

    public: // select handler
        virtual void on_read_event();

        virtual void on_write_event();

        virtual void set_event_handler(EventHandler *eventHandler);

    public:
        const Socket * get_socket() const;

    private:
        Channel channel_;
        Socket *socket_;
        EventHandler *handler_;
    };
}

#endif
