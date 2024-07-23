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
        virtual void on_opened(FtpFileStreamSession *fs);

        virtual void on_read(Connection *conn);

        //FtpFileInfo *info;
    };
}

#endif