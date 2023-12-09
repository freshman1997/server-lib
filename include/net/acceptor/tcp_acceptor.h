#ifndef __TCP_ACCEPTOR_H__
#define __TCP_ACCEPTOR_H__

#include "net/acceptor/acceptor.h"
namespace net
{
    class TcpAcceptor : public Acceptor
    {
    public:
        virtual void listen();

        virtual void on_close();

        virtual void set_handler(AcceptHandler *acceptor);
        
    private:
        AcceptHandler *handler_;
    };
}

#endif
