#ifndef NET_FTP_SERVER_COMMAND_FEAT_H
#define NET_FTP_SERVER_COMMAND_FEAT_H

#include "server/command.h"

namespace yuan::net::ftp
{
    class CommandFeat : public Command
    {
    public:
        FtpCommandResponse execute(FtpSession *session, const std::string &args) override;
        CommandType get_command_type() override;
        std::string get_command_name() override;
    };
}

#endif
