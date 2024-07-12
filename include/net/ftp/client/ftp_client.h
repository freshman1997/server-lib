#ifndef __NET_FTP_FTP_CLIENT_H__
#define __NET_FTP_FTP_CLIENT_H__
#include "net/base/handler/connection_handler.h"

namespace net::ftp 
{
    class FtpClient : public ConnectionHandler
    {
    public:
        FtpClient();

    public:
        virtual void on_connected(Connection *conn);

        virtual void on_error(Connection *conn);

        virtual void on_read(Connection *conn);

        virtual void on_write(Connection *conn);

        virtual void on_close(Connection *conn);
    };
}

#endif