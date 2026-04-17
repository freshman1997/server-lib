#ifndef NET_FTP_SERVER_SERVER_SESSION_H
#define NET_FTP_SERVER_SERVER_SESSION_H
#include "common/session.h"
#include "command_parser.h"

namespace yuan::net::ftp
{
    class FtpApp;

    class ServerFtpSession : public FtpSession
    {
    public:
        ServerFtpSession(Connection *conn, FtpApp *app, bool keepUtilSent = false, bool async_mode = false);
        ~ServerFtpSession();

    public:
        virtual void on_read(Connection *conn);

        FtpCommandParser &command_parser()
        {
            return command_parser_;
        }

    private:
        FtpCommandParser command_parser_;
    };
}

#endif