#ifndef __NET_BASE_UDP_ACCEPTOR_H__
#define __NET_BASE_UDP_ACCEPTOR_H__

namespace net
{
    class Socket;

    class UdpAcceptor
    {
    public:
        UdpAcceptor(Socket *socket);

        ~UdpAcceptor();

        virtual void on_read_event();
    };
}

#endif
