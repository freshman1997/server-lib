#ifndef __ACCEPTOR_H__
#define __ACCEPTOR_H__

#include "net/socket/socket.h"
namespace net
{
    class Channel;

    class Acceptor
    {
    public:
        Acceptor();

        void listen();

        void on_event();

    private:
        Socket *accept_socket_;
        Channel *accept_channel_;
    };
}

#endif
