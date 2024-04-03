#ifndef __TCP_SOCKET_HANDLER_H__
#define __TCP_SOCKET_HANDLER_H__

namespace net
{
    class Connection;

    class ConnectionHandler
    {
    public:
        virtual void on_connected(Connection *conn) = 0;

        virtual void on_error(Connection *conn) = 0;

        virtual void on_read(Connection *conn) = 0;

        virtual void on_write(Connection *conn) = 0;

        virtual void on_close(Connection *conn) = 0;
    };
}

#endif
