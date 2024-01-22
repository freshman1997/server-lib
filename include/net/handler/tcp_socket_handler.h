#ifndef __TCP_SOCKET_HANDLER_H__
#define __TCP_SOCKET_HANDLER_H__

namespace net
{
    class TcpConnection;

    class TcpConnectionHandler
    {
    public:
        virtual void on_connected(TcpConnection *conn) = 0;

        virtual void on_error(TcpConnection *conn) = 0;

        virtual void on_read(TcpConnection *conn) = 0;

        virtual void on_wirte(TcpConnection *conn) = 0;

        virtual void on_close(TcpConnection *conn) = 0;
    };
}

#endif
