#ifndef __NET_FTP_SERVER_SERVER_SESSION_H__
#define __NET_FTP_SERVER_SERVER_SESSION_H__
#include "common/session.h"
#include "command_parser.h"

namespace yuan::net::ftp 
{
    class FtpApp;

    class ServerFtpSession : public FtpSession
    {
    public:
        ServerFtpSession(Connection *conn, FtpApp *entry, bool keepUtilSent = false);
        ~ServerFtpSession();

    public:
        virtual void on_read(Connection *conn);

    private:
        FtpCommandParser command_parser_;
    };
}

#endif