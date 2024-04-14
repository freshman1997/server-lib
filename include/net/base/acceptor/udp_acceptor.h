#ifndef __NET_BASE_UDP_ACCEPTOR_H__
#define __NET_BASE_UDP_ACCEPTOR_H__
#include "net/base/acceptor/acceptor.h"
#include "net/base/channel/channel.h"

namespace net
{
    class Socket;

    class UdpAcceptor : public Acceptor
    {
    public:
        UdpAcceptor(Socket *socket);

        ~UdpAcceptor();

        virtual bool listen();

        virtual void on_close();

        virtual Channel * get_channel();

    public: // select handler
        virtual void on_read_event();

        virtual void on_write_event();

        virtual void set_event_handler(EventHandler *eventHandler);

    private:
        Channel channel_;
        Socket *sock_;
        EventHandler *handler_;
    };
}

#endif
