#ifndef __ACCEPTOR_H__
#define __ACCEPTOR_H__

namespace net
{
    class AcceptHandler;
    class Connection;

    class Acceptor
    {
    public:
        virtual bool listen() = 0;

        virtual void on_close() = 0;

        virtual void set_handler(AcceptHandler *acceptor) = 0;

        virtual void on_new_connection(Connection *conn) = 0;

    };
}

#endif
