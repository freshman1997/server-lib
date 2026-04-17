#ifndef NET_FTP_CLIENT_COMMAND_SCANNER_H
#define NET_FTP_CLIENT_COMMAND_SCANNER_H
#include <string>

namespace yuan::net::ftp
{
    class CommandScanner
    {
    public:
        static std::string simpleCommand();
    };
}

#endif