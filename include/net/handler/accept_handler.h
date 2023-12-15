#ifndef __ACCEPT_HANDLER_H__
#define __ACCEPT_HANDLER_H__

namespace net
{
    class Connection;
    class Acceptor;

    class AcceptHandler
    {
    public:
        virtual void on_new_connection(Connection *conn, Acceptor *acceptor) = 0;

        virtual void on_close(Acceptor *acceptor) = 0;
    };
}

#endif