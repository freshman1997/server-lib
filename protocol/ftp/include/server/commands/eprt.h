#ifndef NET_FTP_SERVER_COMMAND_EPRT_H
#define NET_FTP_SERVER_COMMAND_EPRT_H

#include "../command.h"

namespace yuan::net::ftp
{
    class CommandEprt : public Command
    {
    public:
        FtpCommandResponse execute(FtpSession *session, const std::string &args) override;
        CommandType get_command_type() override;
        std::string get_command_name() override;
    };
}

#endif
