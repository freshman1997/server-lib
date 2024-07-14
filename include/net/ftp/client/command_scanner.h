#ifndef _NET_FTP_CLIENT_COMMAND_SCANNER_H__
#define _NET_FTP_CLIENT_COMMAND_SCANNER_H__
#include <string>

namespace net::ftp 
{
    class CommandScanner
    {
    public:
        static std::string simpleCommand();
    };
}

#endif