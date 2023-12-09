#ifndef __ACCEPTOR_H__
#define __ACCEPTOR_H__

namespace net
{
    class AcceptHandler;

    class Acceptor
    {
    public:
        virtual void listen() = 0;

        virtual void on_close() = 0;

        virtual void set_handler(AcceptHandler *acceptor) = 0;
    };
}

#endif
