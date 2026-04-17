#ifndef NET_FTP_CLIENT_CONTEXT_H
#define NET_FTP_CLIENT_CONTEXT_H

#include <string>

namespace yuan::net::ftp
{
    struct FtpClientResponse
    {
        int code_ = 0;
        std::string body_;
    };
}

#endif
