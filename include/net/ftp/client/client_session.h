#ifndef __NET_FTP_CLIENT_CLIENT_SESSION_H__
#define __NET_FTP_CLIENT_CLIENT_SESSION_H__
#include "../common/session.h"

namespace net::ftp 
{
    class FtpApp;

    class ClientFtpSession : public FtpSession
    {
    public:
        ClientFtpSession(Connection *conn, FtpApp *entry, bool keepUtilSent = false);
        ~ClientFtpSession();

    public:
        void on_read(Connection *conn);
    };
}

#endif