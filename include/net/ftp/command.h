#ifndef __NET_FTP_COMMAND_H__
#define __NET_FTP_COMMAND_H__
#include <string>

namespace net::ftp 
{
    class FtpSession;

    class Command
    {
    public:
        virtual int execute(FtpSession *session, std::string &args) = 0;
    };
}

#endif