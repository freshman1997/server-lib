#ifndef NET_FTP_SERVER_COMMAND_OPTS_H
#define NET_FTP_SERVER_COMMAND_OPTS_H

#include "server/command.h"

namespace yuan::net::ftp
{
    class CommandOpts : public Command
    {
    public:
        FtpCommandResponse execute(FtpSession *session, const std::string &args) override;
        CommandType get_command_type() override;
        std::string get_command_name() override;
    };
}

#endif
